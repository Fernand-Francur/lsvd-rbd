/*
 * file:        lsvd_rbd.cc
 * description: userspace block-on-object layer with librbd interface
 */

#include "extent.cc"
#include "objects.cc"
#include "journal2.cc"

#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <stack>
#include <map>
#include <thread>
#include <ios>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#include <random>
#include <algorithm>

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range
std::random_device rd;     // only used once to initialise (seed) engine
//std::mt19937 rng(rd());  // random-number engine used (Mersenne-Twister in this case)
std::mt19937 rng(17);      // for deterministic testing

typedef int64_t lba_t;
typedef int64_t sector_t;

/* make this atomic? */
int batch_seq;
int last_ckpt;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

static int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}
static int round_up(int n, int m)
{
    return m * div_round_up(n, m);
}

size_t iov_sum(iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
	sum += iov[i].iov_len;
    return sum;
}

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

std::mutex   m; 		// for now everything uses one mutex

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) = 0;
    virtual std::string object_name(int seq) = 0;
    virtual ~backend(){}
};

struct batch {
    char  *buf;
    size_t max;
    size_t len;
    int    seq;
    std::vector<data_map> entries;
public:
    batch(size_t _max) {
	buf = (char*)malloc(_max);
	max = _max;
    }
    ~batch(){
	free((void*)buf);
    }
    void reset(void) {
	len = 0;
	entries.resize(0);
	seq = batch_seq++;
    }
    void append_iov(uint64_t lba, iovec *iov, int iovcnt) {
	char *ptr = buf + len;
	for (int i = 0; i < iovcnt; i++) {
	    memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
	    entries.push_back((data_map){lba, iov[i].iov_len / 512});
	    ptr += iov[i].iov_len;
	    len += iov[i].iov_len;
	    lba += iov[i].iov_len / 512;
	}
    }
    int hdrlen(void) {
	return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }
};

template <class T>
class thread_pool {
public:
    std::queue<T> q;
    bool         running;
    std::mutex  *m;
    std::condition_variable cv;
    std::queue<std::thread> pool;
    
    thread_pool(std::mutex *_m) {
	running = true;
	m = _m;
    }
    ~thread_pool() {
	std::unique_lock lk(*m);
	running = false;
	cv.notify_all();
	lk.unlock();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }	
    T get_locked(std::unique_lock<std::mutex> &lk) {
	while (running && q.empty())
	    cv.wait(lk);
	auto val = q.front();
	q.pop();
	return val;
    }
    bool wait_locked(std::unique_lock<std::mutex> &lk) {
	while (running && q.empty())
	    cv.wait(lk);
	return running;
    }
    bool get_nowait(T &val) {
	if (!running || q.empty())
	    return false;
	val = q.front();
	q.pop();
	return true;
    }
    T get(void) {
	std::unique_lock<std::mutex> lk(*m);
	return get_locked();
    }
    void put_locked(T work) {
	q.push(work);
	cv.notify_one();
    }
    void put(T work) {
	std::unique_lock<std::mutex> lk(*m);
	put_locked(work);
    }
};

/* these all should probably be combined with the stuff in objects.cc to create
 * object classes that serialize and de-serialize themselves. Sometime, maybe.
 */
template <class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals) {
    T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
    for (; p < end; p++)
	vals.push_back(*p);
}


class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};


class translate {
    // mutex protects batch, thread pools, object_info, in_mem_objects
    std::mutex   m;
    objmap      *map;
    
    batch              *current_batch;
    std::stack<batch*>  batches;
    std::map<int,char*> in_mem_objects;

    /* info on all live objects - all sizes in sectors
     */
    struct obj_info {
	uint32_t hdr;
	uint32_t data;
	uint32_t live;
	int      type;
    };
    std::map<int,obj_info> object_info;

    char      *super;
    hdr       *super_h;
    super_hdr *super_sh;
    size_t     super_len;

    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;
    
    char *read_object_hdr(const char *name, bool fast) {
	hdr *h = (hdr*)malloc(4096);
	if (io->read_object(name, (char*)h, 0, 4096) < 0)
	    goto fail;
	if (fast)
	    return (char*)h;
	if (h->hdr_sectors > 8) {
	    h = (hdr*)realloc(h, h->hdr_sectors * 512);
	    if (io->read_object(name, (char*)h, 0, h->hdr_sectors*512) < 0)
		goto fail;
	}
	return (char*)h;
    fail:
	free((char*)h);
	return NULL;
    }

    /* clone_info is variable-length, so we need to pass back pointers 
     * rather than values. That's OK because we allocate superblock permanently
     */
    typedef clone_info *clone_p;
    ssize_t read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps) {
	super = read_object_hdr(name, false);
	super_h = (hdr*)super;
	super_len = super_h->hdr_sectors * 512;

	if (super_h->magic != LSVD_MAGIC || super_h->version != 1 ||
	    super_h->type != LSVD_SUPER)
	    return -1;
	memcpy(my_uuid, super_h->vol_uuid, sizeof(uuid_t));

	super_sh = (super_hdr*)(super_h+1);

	decode_offset_len<uint32_t>(super, super_sh->ckpts_offset, super_sh->ckpts_len, ckpts);
	decode_offset_len<snap_info>(super, super_sh->snaps_offset, super_sh->snaps_len, snaps);

	// this one stores pointers, not values...
	clone_info *p_clone = (clone_info*)(super + super_sh->clones_offset),
	    *end_clone = (clone_info*)(super + super_sh->clones_offset + super_sh->clones_len);
	for (; p_clone < end_clone; p_clone++)
	    clones.push_back(p_clone);

	return super_sh->vol_size * 512;
    }

    ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		       std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap) {
	auto name = io->object_name(seq);
	char *buf = read_object_hdr(name.c_str(), false);
	if (buf == NULL)
	    return -1;
	hdr      *tmp_h = (hdr*)buf;
	data_hdr *tmp_dh = (data_hdr*)(tmp_h+1);
	if (tmp_h->type != LSVD_DATA) {
	    free(buf);
	    return -1;
	}

	h = *tmp_h;
	dh = *tmp_dh;

	decode_offset_len<uint32_t>(buf, tmp_dh->ckpts_offset, tmp_dh->ckpts_len, ckpts);
	decode_offset_len<obj_cleaned>(buf, tmp_dh->objs_cleaned_offset, tmp_dh->objs_cleaned_len, cleaned);
	decode_offset_len<data_map>(buf, tmp_dh->map_offset, tmp_dh->map_len, dmap);

	free(buf);
	return 0;
    }

    ssize_t read_checkpoint(int seq, std::vector<uint32_t> &ckpts, std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes, std::vector<ckpt_mapentry> &dmap) {
	auto name = io->object_name(seq);
	char *buf = read_object_hdr(name.c_str(), false);
	if (buf == NULL)
	    return -1;
	hdr      *h = (hdr*)buf;
	ckpt_hdr *ch = (ckpt_hdr*)(h+1);
	if (h->type != LSVD_CKPT) {
	    free(buf);
	    return -1;
	}

	decode_offset_len<uint32_t>(buf, ch->ckpts_offset, ch->ckpts_len, ckpts);
	decode_offset_len<ckpt_obj>(buf, ch->objs_offset, ch->objs_len, objects);
	decode_offset_len<deferred_delete>(buf, ch->deletes_offset, ch->deletes_len, deletes);
	decode_offset_len<ckpt_mapentry>(buf, ch->map_offset, ch->map_len, dmap);

	free(buf);
	return 0;
    }

    /* TODO: object list
     */
    int write_checkpoint(int seq) {
	std::vector<ckpt_mapentry> entries;
	std::vector<ckpt_obj> objects;
	
	std::unique_lock lk(map->m);
	last_ckpt = seq;
	for (auto it = map->map.begin(); it != map->map.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
			.obj = (int32_t)ptr.obj, .offset = (int32_t)ptr.offset});
	}
	lk.unlock();
	size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

	std::unique_lock lk2(m);
	for (auto it = object_info.begin(); it != object_info.end(); it++) {
	    auto obj_num = it->first;
	    auto [hdr, data, live, type] = it->second;
	    if (type == LSVD_DATA)
		objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num, .hdr_sectors = hdr,
			    .data_sectors = data, .live_sectors = live});
	}

	size_t objs_bytes = objects.size() * sizeof(ckpt_obj);
	size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
	uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes + objs_bytes, 512);
	object_info[seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0, .type = LSVD_CKPT};
	lk2.unlock();

	char *buf = (char*)calloc(hdr_bytes, 1);
	hdr *h = (hdr*)buf;
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_CKPT, .seq = (uint32_t)seq, .hdr_sectors = sectors,
		   .data_sectors = 0};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
	ckpt_hdr *ch = (ckpt_hdr*)(h+1);

	uint32_t o1 = sizeof(hdr)+sizeof(ckpt_hdr), o2 = o1 + sizeof(seq), o3 = o2 + objs_bytes;
	*ch = (ckpt_hdr){.ckpts_offset = o1, .ckpts_len = sizeof(seq),
			 .objs_offset = o2, .objs_len = o3-o2,
			 .deletes_offset = 0, .deletes_len = 0,
			 .map_offset = o3, .map_len = (uint32_t)map_bytes};

	iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		       {.iov_base = (char*)&seq, .iov_len = sizeof(seq)},
		       {.iov_base = (char*)objects.data(), objs_bytes},
		       {.iov_base = (char*)entries.data(), map_bytes}};
	io->write_numbered_object(seq, iov, 4);
	return seq;
    }
    
    int make_hdr(char *buf, batch *b) {
	hdr *h = (hdr*)buf;
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0}, .type = LSVD_DATA,
		   .seq = (uint32_t)b->seq, .hdr_sectors = 8, .data_sectors = (uint32_t)(b->len / 512)};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

	data_hdr *dh = (data_hdr*)(h+1);
	uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t), o2 = o1 + l1,
	    l2 = b->entries.size() * sizeof(data_map);
	*dh = (data_hdr){.last_data_obj = (uint32_t)b->seq, .ckpts_offset = o1, .ckpts_len = l1,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .map_offset = o2, .map_len = l2};

	uint32_t *p_ckpt = (uint32_t*)(dh+1);
	*p_ckpt = last_ckpt;

	data_map *dm = (data_map*)(p_ckpt+1);
	for (auto e : b->entries)
	    *dm++ = e;

	return (char*)dm - (char*)buf;
    }

    void worker_thread(thread_pool<batch*> *p) {
	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    auto b = p->get_locked(lk);
	    if (!p->running)
		return;
	    uint32_t hdr_sectors = div_round_up(b->hdrlen(), 512);
	    object_info[b->seq] = (obj_info){.hdr = hdr_sectors, .data = (uint32_t)(b->len/512),
					     .live = (uint32_t)(b->len/512), .type = LSVD_DATA};
	    lk.unlock();

	    char *hdr = (char*)calloc(hdr_sectors*512, 1);
	    make_hdr(hdr, b);
	    iovec iov[2] = {{hdr, (size_t)(hdr_sectors*512)}, {b->buf, b->len}};
	    io->write_numbered_object(b->seq, iov, 2);
	    free(hdr);

	    lk.lock();
	    std::unique_lock objlock(map->m);
	    auto offset = hdr_sectors;
	    for (auto e : b->entries) {
		std::vector<extmap::lba2obj> deleted;
		extmap::obj_offset oo = {b->seq, offset};
		map->map.update(e.lba, e.lba+e.len, oo, &deleted);
		for (auto d : deleted) {
		    auto [base, limit, ptr] = d.vals();
		    if (ptr.obj != b->seq)
			object_info[ptr.obj].live -= (limit - base);
		}
	    }
	    in_mem_objects.erase(b->seq);
	    batches.push(b);
	    objlock.unlock();
	    lk.unlock();
	}
    }

    void ckpt_thread(thread_pool<int> *p) {
	auto one_second = std::chrono::seconds(1);
	auto seq0 = batch_seq;
	const int ckpt_interval = 100;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    p->cv.wait_for(lk, one_second);
	    if (p->running && batch_seq - seq0 > ckpt_interval) {
		seq0 = batch_seq;
		lk.unlock();
		checkpoint();
	    }
	}
    }

    void flush_thread(thread_pool<int> *p) {
	auto wait_time = std::chrono::milliseconds(500);
	auto timeout = std::chrono::seconds(2);
	auto t0 = std::chrono::system_clock::now();
	auto seq0 = batch_seq;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(*p->m);
	    p->cv.wait_for(lk, wait_time);
	    if (p->running && current_batch && seq0 == batch_seq && current_batch->len > 0) {
		if (std::chrono::system_clock::now() - t0 > timeout) {
		    lk.unlock();
		    flush();
		}
	    }
	    else {
		seq0 = batch_seq;
		t0 = std::chrono::system_clock::now();
	    }
	}
    }

public:
    backend *io;
    bool     nocache;
    
    translate(backend *_io, objmap *omap) : workers(&m), misc_threads(&m) {
	io = _io;
	map = omap;
	current_batch = NULL;
    }
    ~translate() {
	while (!batches.empty()) {
	    auto b = batches.top();
	    batches.pop();
	    delete b;
	}
	if (current_batch)
	    delete current_batch;
	free(super);
    }
    
    /* returns sequence number of ckpt
     */
    int checkpoint(void) {
	std::unique_lock<std::mutex> lk(m);
	if (current_batch && current_batch->len > 0) {
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	int seq = batch_seq++;
	lk.unlock();
	return write_checkpoint(seq);
    }

    int flush(void) {
	std::unique_lock<std::mutex> lk(m);
	int val = 0;
	if (current_batch && current_batch->len > 0) {
	    val = current_batch->seq;
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	return val;
    }

    ssize_t init(const char *name, int nthreads, bool timedflush) {
	std::vector<uint32_t>  ckpts;
	std::vector<clone_p>   clones;
	std::vector<snap_info> snaps;
	ssize_t bytes = read_super(name, ckpts, clones, snaps);
	if (bytes < 0)
	  return bytes;
	batch_seq = super_sh->next_obj;

	int _ckpt = 1;
	for (auto ck : ckpts) {
	    ckpts.resize(0);
	    std::vector<ckpt_obj> objects;
	    std::vector<deferred_delete> deletes;
	    std::vector<ckpt_mapentry> entries;
	    if (read_checkpoint(ck, ckpts, objects, deletes, entries) < 0)
		return -1;
	    for (auto o : objects) {
		object_info[o.seq] = (obj_info){.hdr = o.hdr_sectors, .data = o.data_sectors,
					.live = o.live_sectors, .type = LSVD_DATA};

	    }
	    for (auto m : entries) {
		map->map.update(m.lba, m.lba + m.len,
				(extmap::obj_offset){.obj = m.obj, .offset = m.offset});
	    }
	    _ckpt = ck;
	}

	for (int i = _ckpt; ; i++) {
	    std::vector<uint32_t>    ckpts;
	    std::vector<obj_cleaned> cleaned;
	    std::vector<data_map>    entries;
	    hdr h; data_hdr dh;
	    batch_seq = i;
	    if (read_data_hdr(i, h, dh, ckpts, cleaned, entries) < 0)
		break;
	    object_info[i] = (obj_info){.hdr = h.hdr_sectors, .data = h.data_sectors,
					.live = h.data_sectors, .type = LSVD_DATA};
	    int offset = 0, hdr_len = h.hdr_sectors;
	    for (auto m : entries) {
		map->map.update(m.lba, m.lba + m.len,
				(extmap::obj_offset){.obj = i, .offset = offset + hdr_len});
		offset += m.len;
	    }
	}

	for (int i = 0; i < nthreads; i++) 
	    workers.pool.push(std::thread(&translate::worker_thread, this, &workers));
	misc_threads.pool.push(std::thread(&translate::ckpt_thread, this, &misc_threads));
	if (timedflush)
	    misc_threads.pool.push(std::thread(&translate::flush_thread, this, &misc_threads));

	return bytes;
    }

    void shutdown(void) {
    }

    ssize_t writev(size_t offset, iovec *iov, int iovcnt) {
	std::unique_lock<std::mutex> lk(m);
	size_t len = iov_sum(iov, iovcnt);

	if (current_batch && current_batch->len + len > current_batch->max) {
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	if (current_batch == NULL) {
	    if (batches.empty())
		current_batch = new batch(BATCH_SIZE);
	    else {
		current_batch = batches.top();
		batches.pop();
	    }
	    current_batch->reset();
	    if (nocache)
		in_mem_objects[current_batch->seq] = current_batch->buf;
	}

	int64_t sector_offset = current_batch->len / 512,
	    lba = offset/512, limit = (offset+len)/512;
	current_batch->append_iov(offset / 512, iov, iovcnt);

	if (nocache) {
	    std::vector<extmap::lba2obj> deleted;
	    extmap::obj_offset oo = {current_batch->seq, sector_offset};
	    std::unique_lock objlock(map->m);

	    map->map.update(lba, limit, oo, &deleted);
	    for (auto d : deleted) {
		auto [base, limit, ptr] = d.vals();
		object_info[ptr.obj].live -= (limit - base);
	    }
	}
	
	
	return len;
    }

    ssize_t write(size_t offset, size_t len, char *buf) {
	iovec iov = {buf, len};
	return this->writev(offset, &iov, 1);
    }

    ssize_t read(size_t offset, size_t len, char *buf) {
	int64_t base = offset / 512;
	int64_t sectors = len / 512, limit = base + sectors;

	if (map->map.size() == 0) {
	    memset(buf, 0, len);
	    return len;
	}

	/* object number, offset (bytes), length (bytes) */
	std::vector<std::tuple<int, size_t, size_t>> regions;

	std::unique_lock lk(m);
	std::shared_lock slk(map->m);
	
	auto prev = base;
	char *ptr = buf;
	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (_base > prev) {	// unmapped
		size_t _len = (_base - prev)*512;
		regions.push_back(std::tuple(-1, 0, _len));
		ptr += _len;
	    }
	    size_t _len = (_limit - _base) * 512,
		_offset = oo.offset * 512;
	    int obj = oo.obj;
	    if (in_mem_objects.find(obj) != in_mem_objects.end()) {
		memcpy((void*)ptr, in_mem_objects[obj]+_offset, _len);
		obj = -2;
	    }
	    regions.push_back(std::tuple(obj, _offset, _len));
	    ptr += _len;
	    prev = _limit;
	}
	slk.unlock();
	lk.unlock();

	ptr = buf;
	for (auto [obj, _offset, _len] : regions) {
	    if (obj == -1)
		memset(ptr, 0, _len);
	    else if (obj == -2)
		/* skip */;
	    else
		io->read_numbered_object(obj, ptr, _offset, _len);
	    ptr += _len;
	}

	return ptr - buf;
    }

    // debug methods
    int inmem(int max, int *list) {
	int i = 0;
	for (auto it = in_mem_objects.begin(); i < max && it != in_mem_objects.end(); it++)
	    list[i++] = it->first;
	return i;
    }
    void getmap(int base, int limit, int (*cb)(void *ptr,int,int,int,int), void *ptr) {
	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
		break;
	}
    }
    int mapsize(void) {
	return map->map.size();
    }
    void reset(void) {
	map->map.reset();
    }
    int frontier(void) {
	if (current_batch)
	    return current_batch->len / 512;
	return 0;
    }
};

void throw_fs_error(std::string msg) {
    throw fs::filesystem_error(msg, std::error_code(errno, std::system_category()));
}

/* the read cache is:
 * 1. indexed by obj/offset[*], not LBA
 * 2. stores aligned 64KB blocks 
 * 3. tolerates 4KB-aligned "holes" using a per-entry bitmap (64KB = 16 bits)
 * [*] offset is in units of 64KB blocks
 */
class read_cache {
    std::mutex m;
    std::map<extmap::obj_offset,int> map;

    j_read_super       *super;
    extmap::obj_offset *flat_map;
    uint16_t           *bitmap;
    objmap             *omap;
    translate          *be;
    int                 fd;
    backend            *io;
    
    int               unit_sectors;
    std::vector<int>  free_blks;
    bool              map_dirty;
    
    thread_pool<int> misc_threads; // eviction thread, for now
    bool             nothreads;	   // for debug
    
    // cache block is busy when being written - mask can change, but not
    // its mapping.
    std::vector<bool> busy;
    std::condition_variable cv;

    /* this is kind of crazy. return a bitmap corresponding to the 4KB pages
     * in [base%unit, limit%unit), where base, limit, and unit are in sectors
     */
    typedef uint16_t mask_t;
    mask_t page_mask(int base, int limit, int unit) {
	int top = round_up(base+1, unit);
	limit = (limit < top) ? limit : top;
	int base_page = base/8, limit_page = div_round_up(limit, 8),
	    unit_page = unit / 8;
	mask_t val = 0;
	for (int i = base_page % unit_page; base_page < limit_page; base_page++, i++)
	    val |= (1 << i);
	return val;
    }

    /* evict 'n' blocks
     */
    void evict(int n) {
	assert(!m.try_lock());	// m must be locked
	std::uniform_int_distribution<int> uni(0,super->units - 1);
	for (int i = 0; i < n; i++) {
	    int j = uni(rng);
	    bitmap[j] = 0;
	    auto oo = flat_map[j];
	    flat_map[j] = (extmap::obj_offset){0, 0};
	    map.erase(oo);
	    free_blks.push_back(j);
	}
    }

    void evict_thread(thread_pool<int> *p) {
	auto wait_time = std::chrono::seconds(2);
	auto t0 = std::chrono::system_clock::now();
	auto timeout = std::chrono::seconds(15);

	std::unique_lock<std::mutex> lk(m);

	while (p->running) {
	    while (p->running)
		p->cv.wait_for(lk, wait_time);
	    if (!p->running)
		return;

	    if (!map_dirty)	// free list didn't change
		continue;

	    int n = 0;
	    if ((int)free_blks.size() < super->units / 16)
		n = super->units / 4 - free_blks.size();
	    if (n)
		evict(n);

	    /* write the map (a) immediately if we evict something, or 
	     * (b) occasionally if the map is dirty
	     */
	    auto t = std::chrono::system_clock::now();
	    if (n > 0 || t - t0 > timeout) {
		lk.unlock();
		pwrite(fd, flat_map, 4096 * super->map_blocks, 4096 * super->map_start);
		pwrite(fd, bitmap, 4096 * super->bitmap_blocks, 4096 * super->bitmap_start);
		lk.lock();
	    }
	}
    }
    
public:    
    
    // TODO: iovec version
    void add(extmap::obj_offset oo, int sectors, char *buf) {
	// must be 4KB aligned
	assert(!(oo.offset & 7)); 
	
	while (sectors > 0) {
	    std::unique_lock lk(m);

	    extmap::obj_offset obj_blk = {oo.obj, oo.offset / unit_sectors};
	    int cache_blk = -1;
	    auto it = map.find(obj_blk);
	    if (it != map.end())
		cache_blk = it->second;
	    else if (free_blks.size() > 0) {
		cache_blk = free_blks.back();
		free_blks.pop_back();
	    }
	    else
		return;
	    while (busy[cache_blk])
		cv.wait(lk);
	    busy[cache_blk] = true;
	    auto mask = bitmap[cache_blk];
	    lk.unlock();

	    assert(cache_blk >= 0);
	    
	    auto obj_page = oo.offset / 8;
	    auto pages_in_blk = unit_sectors / 8;
	    auto blk_page = obj_blk.offset * pages_in_blk;
	    std::vector<iovec> iov;
	    
	    for (int i = obj_page - blk_page; sectors > 0 && i < pages_in_blk; i++) {
		mask |= (1 << i);
		iov.push_back((iovec){buf, 4096});
		buf += 4096;
		sectors -= 8;
		oo.offset += 8;
	    }

	    off_t blk_offset = ((cache_blk * pages_in_blk) + super->base) * 4096;
	    blk_offset += (obj_page - blk_page) * 4096;
	    if (pwritev(fd, iov.data(), iov.size(), blk_offset) < 0)
		throw_fs_error("rcache");

	    lk.lock();
	    map[obj_blk] = cache_blk;
	    bitmap[cache_blk] = mask;
	    flat_map[cache_blk] = obj_blk;
	    busy[cache_blk] = false;
	    map_dirty = true;
	    cv.notify_one();
	    lk.unlock();
	}
    }

    /* eventual implementation (full async):
     * - cache device uses io_uring, fire writes from here
     * - worker threads for backend reads
     * - some sort of reference count structure for completion
     */
    void read(size_t offset, size_t len, char *buf) {
	lba_t lba = offset/512, sectors = len/512;
	std::vector<std::tuple<lba_t,lba_t,extmap::obj_offset>> extents;
	std::shared_lock lk(omap->m);
	for (auto it = omap->map.lookup(lba);
	     it != omap->map.end() && it->base() < lba+sectors; it++) 
	    extents.push_back(it->vals(lba, lba+sectors));
	lk.unlock();

	std::vector<std::tuple<extmap::obj_offset,sector_t,char*>> to_add;
	
	for (auto e : extents) {
	    assert(len > 0);
	    
	    auto [base, limit, ptr] = e;
	    if (base > lba) {
		auto bytes = (base-lba)*512;
		memset(buf, 0, bytes);
		buf += bytes;
		len -= bytes;
	    }
	    while (base < limit) {
		extmap::obj_offset unit = {ptr.obj, ptr.offset / unit_sectors};

                // TODO: unit_sectors -> unit_nsectors
		sector_t blk_base_lba = unit.offset * unit_sectors;
                sector_t blk_offset = ptr.offset % unit_sectors;
                sector_t blk_top_offset = std::min({(int)(blk_offset+sectors),
			    round_up(blk_offset+1,unit_sectors),
			    (int)(blk_offset + (limit-base))});
		
		bool in_cache = false;
		int n = -1;
		auto it = map.find(unit);
		if (it != map.end()) {
		    n = it->second;
		    mask_t access_mask = page_mask(blk_offset, blk_top_offset, unit_sectors);
		    if ((access_mask & bitmap[n]) == access_mask)
			in_cache = true;
		}

		
		if (in_cache) {
		    sector_t blk_in_ssd = super->base*8 + n*unit_sectors,
			start = blk_in_ssd + blk_offset,
			finish = start + (blk_top_offset - blk_offset);

		    if (pread(fd, buf, 512*(finish-start), 512*start) < 0)
			throw_fs_error("rcache");
		    base += (finish - start);
		    ptr.offset += (finish - start); // HACK
		    buf += 512*(finish-start);
		    len -= 512*(finish-start);
		}
		else {
		    char *cache_line = (char*)aligned_alloc(512, unit_sectors*512);
		    auto bytes = io->read_numbered_object(unit.obj, cache_line,
							  512*blk_base_lba, 512*unit_sectors);
                    size_t start = 512 * blk_offset,
			finish = 512 * blk_top_offset;
		    assert((int)finish <= bytes);

		    memcpy(buf, cache_line+start, (finish-start));

		    base += (blk_top_offset - blk_offset);
		    ptr.offset += (blk_top_offset - blk_offset); // HACK
		    buf += (finish-start);
		    len -= (finish-start);
		    extmap::obj_offset ox = {unit.obj, unit.offset * unit_sectors};
		    to_add.push_back(std::tuple(ox, bytes/512, cache_line));
		}
	    }
	    lba = limit;
	}
	if (len > 0)
	    memset(buf, 0, len);

	// now read is finished, and we can add shit to the cache
	for (auto [oo, n, cache_line] : to_add) {
	    add(oo, n, cache_line);
	    free(cache_line);
	}
    }

    read_cache(uint32_t blkno, int _fd, bool nt, translate *_be, objmap *_om, backend *_io) :
	omap(_om), be(_be), fd(_fd), io(_io), misc_threads(&m), nothreads(nt)
    {
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096*blkno) < 4096)
	    throw_fs_error("rcache");
	super = (j_read_super*)buf;
	
	assert(super->unit_size == 128); // 64KB, in sectors
	unit_sectors = super->unit_size; // todo: fixme
	
	int oos_per_pg = 4096 / sizeof(extmap::obj_offset);
	assert(div_round_up(super->units, oos_per_pg) == super->map_blocks);
	assert(div_round_up(super->units, 2048) == super->bitmap_blocks);

	flat_map = (extmap::obj_offset*)aligned_alloc(512, super->map_blocks*4096);
	if (pread(fd, (char*)flat_map, super->map_blocks*4096, super->map_start*4096) < 0)
	    throw_fs_error("rcache2");
	bitmap = (uint16_t*)aligned_alloc(512, super->bitmap_blocks*4096);
	if (pread(fd, (char*)bitmap, super->bitmap_blocks*4096, super->bitmap_start*4096) < 0)
	    throw_fs_error("rcache3");

	for (int i = 0; i < super->units; i++)
	    if (flat_map[i].obj != 0)
		map[flat_map[i]] = i;
	    else {
		free_blks.push_back(i);
		bitmap[i] = 0;
	    }

	busy.reserve(super->units);
	for (int i = 0; i < super->units; i++)
	    busy.push_back(false);
	map_dirty = false;

	if (!nothreads)
	    misc_threads.pool.push(std::thread(&read_cache::evict_thread, this, &misc_threads));
    }

    ~read_cache() {
	free((void*)flat_map);
	free((void*)bitmap);
	free((void*)super);
    }

    /* debugging
     */
    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, uint16_t **p_bitmap,
		  std::vector<int> **p_free_blks, std::map<extmap::obj_offset,int> **p_map) {
	if (p_super != NULL)
	    *p_super = super;
	if (p_flat != NULL)
	    *p_flat = flat_map;
	if (p_bitmap != NULL)
	    *p_bitmap = bitmap;
	if (p_free_blks != NULL)
	    *p_free_blks = &free_blks;
	if (p_map != NULL)
	    *p_map = &map;
    }

    void do_evict(int n) {
	std::unique_lock lk(m);
	evict(n);
    }
    void reset(void) {
    }
    
};

/* simple backend that uses files in a directory. 
 * good for debugging and testing
 */
class file_backend : public backend {
    char *prefix;
public:
    file_backend(const char *_prefix) {
	prefix = strdup(_prefix);
    }
    ~file_backend() {
	free((void*)prefix);
    }
    ssize_t write_object(const char *name, iovec *iov, int iovcnt) {
	int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	    return -1;
	auto val = writev(fd, iov, iovcnt);
	close(fd);
	return val;
    }
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) {
	auto name = std::string(prefix) + "." + hex(seq);
	return write_object(name.c_str(), iov, iovcnt);
    }
    ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) {
	int fd = open(name, O_RDONLY);
	if (fd < 0)
	    return -1;
	auto val = pread(fd, buf, len, offset);
	close(fd);
	return val;
    }
    ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) {
	auto name = std::string(prefix) + "." + hex(seq);
	return read_object(name.c_str(), buf, offset, len);
    }
    std::string object_name(int seq) {
	return std::string(prefix) + "." + hex(seq);
    }
};    


/* each write queues up one of these for a worker thread
 */
struct wcache_work {
    uint64_t  lba;
    iovec    *iov;
    int       iovcnt;
    void    (*callback)(void*);
    void     *ptr;
};

static bool aligned(void *ptr, int a)
{
    return 0 == ((long)ptr & (a-1));
}

#if 0
static void iov_consume(iovec *iov, int iovcnt, size_t nbytes)
{
    for (int i = 0; i < iovcnt && nbytes > 0; i++) {
	if (iov[i].iov_len < nbytes) {
	    nbytes -= iov[i].iov_len;
	    iov[i].iov_len = 0;
	}
	else {
	    iov[i].iov_len -= nbytes;
	    iov[i].iov_base = (char*)iov[i].iov_base + nbytes;
	    nbytes = 0;
	}
    }
}

static void iov_zero(iovec *iov, int iovcnt, size_t nbytes)
{
    for (int i = 0; i < iovcnt && nbytes > 0; i++) {
	if (iov[i].iov_len < nbytes) {
	    memset(iov[i].iov_base, 0, iov[i].iov_len);
	    nbytes -= iov[i].iov_len;
	    iov[i].iov_len = 0;
	}
	else {
	    memset(iov[i].iov_base, 0, iov[i].iov_len);
	    iov[i].iov_len -= nbytes;
	    iov[i].iov_base = (char*)iov[i].iov_base + nbytes;
	    nbytes = 0;
	}
    }
}
#endif

typedef int page_t;

/* all addresses are in units of 4KB blocks
 */
class write_cache {
    int            fd;
    uint32_t       super_blkno;
    j_write_super *super;	// 4KB

    extmap::cachemap2 map;
    translate        *be;
    
    thread_pool<wcache_work> workers;
    thread_pool<int>         misc_threads;
    std::mutex               m;

    char *pad_page;
    
    static const int n_threads = 1;

    /* lock must be held before calling
     */
    uint32_t allocate(page_t n, page_t &pad) {
	pad = 0;
	if (super->limit - super->next < (uint32_t)n) {
	    pad = super->next;
	    super->next = 0;
	}
	auto val = super->next;
	super->next += n;
	return val;
    }

    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks) {
	memset(buf, 0, 4096);
	j_hdr *h = (j_hdr*)buf;
	*h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1, .vol_uuid = {0},
		     .seq = super->seq++, .len = (uint32_t)blks, .crc32 = 0,
		     .extent_offset = 0, .extent_len = 0};
	memcpy(h->vol_uuid, uuid, sizeof(uuid));
	return h;
    }

    void writer(thread_pool<wcache_work> *p) {
	char *buf = (char*)aligned_alloc(512, 4096); // for direct I/O
	
	while (p->running) {
	    std::vector<char*> bounce_bufs;
	    std::unique_lock<std::mutex> lk(m);
	    if (!p->wait_locked(lk))
		break;

	    std::vector<wcache_work> work;
	    std::vector<int> lengths;
	    int sectors = 0;
	    while (p->running) {
		wcache_work w;
		if (!p->get_nowait(w))
		    break;
		auto l = iov_sum(w.iov, w.iovcnt) / 512;		
		sectors += l;
		lengths.push_back(l);
		work.push_back(w);
	    }
	    
	    page_t blocks = div_round_up(sectors, 8);
	    // allocate blocks + 1
	    page_t pad, blockno = allocate(blocks+1, pad);
	    lk.unlock();

	    if (pad != 0) {
		mk_header(buf, LSVD_J_PAD, my_uuid, (super->limit - pad));
		if (pwrite(fd, buf, 4096, pad*4096) < 0)
		    /* TODO: do something */;
	    }

	    std::vector<j_extent> extents;
	    for (auto w : work) {
		for (int i = 0; i < w.iovcnt; i++) {
		    if (aligned(w.iov[i].iov_base, 512))
			continue;
		    char *p = (char*)aligned_alloc(512, w.iov[i].iov_len);
		    memcpy(p, w.iov[i].iov_base, w.iov[i].iov_len);
		    w.iov[i].iov_base = p;
		    bounce_bufs.push_back(p);
		}
		extents.push_back((j_extent){w.lba, iov_sum(w.iov, w.iovcnt)/512});
	    }

	    j_hdr *j = mk_header(buf, LSVD_J_DATA, my_uuid, 1+blocks);
	    j->extent_offset = sizeof(*j);
	    size_t e_bytes = extents.size() * sizeof(j_extent);
	    j->extent_len = e_bytes;
	    memcpy((void*)(buf + sizeof(*j)), (void*)extents.data(), e_bytes);
		
	    std::vector<iovec> iovs;
	    iovs.push_back((iovec){buf, 4096});

	    for (auto w : work)
		for (int i = 0; i < w.iovcnt; i++)
		    iovs.push_back(w.iov[i]);
		    
	    sector_t pad_sectors = blocks*8 - sectors;
	    if (pad_sectors > 0)
		iovs.push_back((iovec){pad_page, (size_t)pad_sectors*512});

	    if (pwritev(fd, iovs.data(), iovs.size(), blockno*4096) < 0)
		/* TODO: do something */;

	    /* update map first under lock. 
	     * Note that map is in units of *sectors*, not blocks 
	     */
	    lk.lock();
	    uint64_t lba = (blockno+1) * 8;
	    for (auto w : work) {
		auto sectors = iov_sum(w.iov, w.iovcnt) / 512;
		map.update(w.lba, w.lba + sectors, lba);
		lba += sectors;
	    }
	    
	    /* then send to backend */
	    lk.unlock();
	    for (auto w : work) {
		be->writev(w.lba*512, w.iov, w.iovcnt);
		w.callback(w.ptr);
	    }

	    while (!bounce_bufs.empty()) {
		free(bounce_bufs.back());
		bounce_bufs.pop_back();
	    }
	}
	free(buf);
    }
    
    /* cache eviction - get info from oldest entry in cache. [should be private]
     *  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */
    page_t get_oldest(page_t blk, std::vector<j_extent> &extents) {
	char *buf = (char*)aligned_alloc(512, 4096);
	j_hdr *h = (j_hdr*)buf;
	
	if (pread(fd, buf, 4096, blk*4096) < 0)
	    throw_fs_error("wcache");
	assert(h->magic == LSVD_MAGIC && h->version == 1);

	auto next_blk = blk + h->len;
	if (next_blk >= super->limit)
	    next_blk = super->base;
	if (h->type == LSVD_J_DATA)
	    decode_offset_len<j_extent>(buf, h->extent_offset, h->extent_len, extents);

	return next_blk;
    }

    /* min free is min(5%, 100MB). Free space:
     *  N = limit - base
     *  oldest == newest : free = N-1
     *  else : free = ((oldest + N) - newest - 1) % N
     */
    void get_exts_to_evict(std::vector<j_extent> &exts_in, page_t pg_base, page_t pg_limit, 
			   std::vector<j_extent> &exts_out) {
	std::unique_lock<std::mutex> lk(m);
	for (auto e : exts_in) {
	    lba_t base = e.lba, limit = e.lba + e.len;
	    for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
		auto [_base, _limit, ptr] = it->vals(base, limit);
		if (pg_base*8 <= ptr && ptr < pg_limit*8)
		    exts_out.push_back((j_extent){.lba = (uint64_t)_base,
				.len = (uint64_t)(_limit-_base)});
	    }
	}
    }
    
    void evict_thread(thread_pool<int> *p) {
	auto period = std::chrono::seconds(1);
	const int evict_min_pct = 5;
	const int evict_max_mb = 100;
	int trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
			       evict_max_mb * (1024*1024/4096));
	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    p->cv.wait_for(lk, period);
	    int N = super->limit - super->base,
		pgs_free = ((super->oldest + N) - super->next - 1) % N;
	    if (p->running && super->oldest != super->next && pgs_free <= trigger) {
		lk.unlock();

		auto oldest = super->oldest;
		std::vector<j_extent> to_delete;
		while (pgs_free < trigger) {
		    std::vector<j_extent> extents;
		    auto tmp = get_oldest(oldest, extents);
		    get_exts_to_evict(extents, oldest, tmp, to_delete);
		    pgs_free -= (tmp - oldest);
		    oldest = tmp;
		}

		/* TODO: read the data and add to read cache
		 */

		lk.lock();
		for (auto e : to_delete)
		    map.trim(e.lba, e.lba + e.len);
	    }
	}
    }
    
    void ckpt_thread(thread_pool<int> *p) {
	auto seq0 = batch_seq;
	auto period = std::chrono::seconds(1);
	const int ckpt_interval = 100;

	while (p->running) {
	    std::unique_lock lk(m);
	    p->cv.wait_for(lk, period);
	    if (p->running && batch_seq - seq0 > ckpt_interval) {
		seq0 = batch_seq;
		lk.unlock();
		write_checkpoint();
	    }
	}
    }

    void write_checkpoint(void) {
	char *buf = (char*)aligned_alloc(512, 4096);
	j_write_super *super_copy = (j_write_super*)aligned_alloc(512, 4096);

	std::unique_lock<std::mutex> lk(m);
	size_t ckpt_bytes = map.size() * sizeof(j_map_extent);
	page_t ckpt_pages = div_round_up(ckpt_bytes, 4096);
	page_t pad, blockno = allocate(ckpt_pages+1, pad);

	std::vector<j_map_extent> extents;
	for (auto it = map.begin(); it != map.end(); it++)
	    extents.push_back((j_map_extent){(uint64_t)it->s.base, (uint64_t)it->s.len,
			(uint32_t)it->s.ptr/8});
	memcpy(super_copy, super, 4096);
	lk.unlock();

	if (pad != 0) {
	    mk_header(buf, LSVD_J_PAD, my_uuid, (super->limit - pad));
	    if (pwrite(fd, buf, 4096, pad*4096) < 0)
		throw_fs_error("wckpt_pad");
	}

	mk_header(buf, LSVD_J_CKPT, my_uuid, 1+ckpt_pages);
	char *e_buf = (char*)aligned_alloc(512, 4096*ckpt_pages); // bounce buffer
	memcpy(e_buf, extents.data(), ckpt_bytes);
	if (ckpt_bytes % 4096)	// make valgrind happy
	    memset(e_buf + ckpt_bytes, 0, 4096 - (ckpt_bytes % 4096));
	
	super_copy->map_start = super->map_start = blockno+1;
	super_copy->map_blocks = super->map_blocks = ckpt_pages;
	super_copy->map_entries = super->map_entries = extents.size();
	
	std::vector<iovec> iovs;
	iovs.push_back((iovec){buf, 4096});
	iovs.push_back((iovec){e_buf, (size_t)4096*ckpt_pages});

	if (pwritev(fd, iovs.data(), iovs.size(), 4096*blockno) < 0)
	    throw_fs_error("wckpt_e");
	if (pwrite(fd, (char*)super_copy, 4096, 4096*super_blkno) < 0)
	    throw_fs_error("wckpt_s");

	free(buf);
	free(super_copy);
	free(e_buf);
    }
    

public:
    write_cache(uint32_t blkno, int _fd, translate *_be) : workers(&m), misc_threads(&m) {
	super_blkno = blkno;
	fd = _fd;
	be = _be;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096*blkno) < 4096)
	    throw_fs_error("wcache");
	super = (j_write_super*)buf;
	pad_page = (char*)aligned_alloc(512, 4096);
	memset(pad_page, 0, 4096);

	if (super->map_entries) {
	    size_t map_bytes = super->map_entries * sizeof(j_map_extent),
		map_bytes_rounded = round_up(map_bytes, 4096);
	    char *map_buf = (char*)aligned_alloc(512, map_bytes_rounded);
	    std::vector<j_map_extent> extents;
	    if (pread(fd, map_buf, map_bytes_rounded, 4096 * super->map_start) < 0)
		throw_fs_error("wcache_map");
	    decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);
	    for (auto e : extents)
		map.update(e.lba, e.lba+e.len, e.page*8);
	    free(map_buf);
	}
	    
	// https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function
	for (auto i = 0; i < n_threads; i++)
	    workers.pool.push(std::thread(&write_cache::writer, this, &workers));
	misc_threads.pool.push(std::thread(&write_cache::evict_thread, this, &misc_threads));
    }
    ~write_cache() {
	free(pad_page);
	free(super);
    }

    void write(size_t offset, iovec *iov, int iovcnt, void (*callback)(void*), void *ptr) {
	workers.put((wcache_work){offset/512, iov, iovcnt, callback, ptr});
    }

    void get_iov_range(size_t off, size_t len, iovec *iov, int iovcnt,
		       std::vector<iovec> &range) {
	int i = 0;
	while (off > iov[i].iov_len)
	    off -= iov[i++].iov_len;
	auto bytes = std::min(len, iov[i].iov_len - off);
	range.push_back((iovec){(char*)(iov[i++].iov_base) + off, bytes});
	len -= bytes;
	while (len > 0 && len >= iov[i].iov_len) {
	    range.push_back(iov[i]);
	    len -= iov[i].iov_len;
	}
	if (len > 0)
	    range.push_back((iovec){iov[i].iov_base, len});
	assert(i <= iovcnt);
    }
    
    /* returns tuples of:
     * offset, len, buf_offset
     */
    typedef std::tuple<size_t,size_t,size_t> cache_miss;

    // TODO: how the hell to handle fragments?
    void readv(size_t offset, iovec *iov, int iovcnt, std::vector<cache_miss> &misses) {
	auto bytes = iov_sum(iov, iovcnt);
	lba_t base = offset/512, limit = base + bytes/512, prev = base;
	std::unique_lock<std::mutex> lk(m);

	size_t buf_offset = 0;
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (_base > prev) {
		size_t bytes = 512 * (_base - prev);
		misses.push_back(std::tuple((size_t)512*prev, bytes, buf_offset));
		buf_offset += bytes;
	    }

	    size_t bytes = 512 * (_limit - _base),
		nvme_offset = 512 * plba;
	    std::vector<iovec> iovs;
	    get_iov_range(buf_offset, bytes, iov, iovcnt, iovs);

	    lk.unlock();
	    if (preadv(fd, iov, iovcnt, nvme_offset) < 0)
		throw_fs_error("wcache_read");
	    lk.lock();

	    buf_offset += bytes;
	    prev = _limit;
	}
	if (prev < limit)
	    misses.push_back(std::tuple(512 * prev, 512 * (limit - prev), buf_offset));
    }
    
    // debugging
    void getmap(int base, int limit, int (*cb)(void*, int, int, int), void *ptr) {
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)plba))
		break;
	}
    }
    void reset(void) {
	map.reset();
    }
    void get_super(j_write_super *s) {
	*s = *super;
    }
    page_t do_get_oldest(page_t blk, std::vector<j_extent> &extents) {
	return get_oldest(blk, extents);
    }
    void do_write_checkpoint(void) {
	write_checkpoint();
    }
};


translate   *lsvd;
write_cache *wcache;
objmap      *omap;
read_cache  *rcache;
backend     *io;

struct tuple {
    int base;
    int limit;
    int obj;			// object map
    int offset;
    int plba;			// write cache map
};
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

extern "C" void wcache_init(uint32_t blkno, int fd);
void wcache_init(uint32_t blkno, int fd)
{
    wcache = new write_cache(blkno, fd, lsvd);
}

extern "C" void wcache_shutdown(void);
void wcache_shutdown(void)
{
    delete wcache;
    wcache = NULL;
}

extern "C" void wcache_write(char* buf, uint64_t offset, uint64_t len);
struct do_write {
public:
    std::mutex m;
    std::condition_variable cv;
    bool done;
    do_write(){done = false;}
};
static void cb(void *ptr)
{
    do_write *d = (do_write*)ptr;
    std::unique_lock<std::mutex> lk(d->m);
    d->done = true;
    d->cv.notify_all();
}
void wcache_write(char *buf, uint64_t offset, uint64_t len)
{
    do_write dw;
    iovec iov = {buf, len};
    std::unique_lock<std::mutex> lk(dw.m);
    wcache->write(offset, &iov, 1, cb, (void*)&dw);
    while (!dw.done)
	dw.cv.wait(lk);
}

extern "C" void wcache_read(char *buf, uint64_t offset, uint64_t len);
void wcache_read(char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    iovec iov = {buf2, len};
    std::vector<write_cache::cache_miss> misses;
    wcache->readv(offset, &iov, 1, misses);

    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" int wcache_getmap(int, int, int, struct tuple*);
int wc_getmap_cb(void *ptr, int base, int limit, int plba)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max)
	s->t[s->i++] = (tuple){base, limit, 0, 0, plba};
    return s->i < s->max;
}
int wcache_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    wcache->getmap(base, limit, wc_getmap_cb, (void*)&s);
    return s.i;
}

extern "C" void wcache_get_super(j_write_super *s);
void wcache_get_super(j_write_super *s)
{
    wcache->get_super(s);
}

extern "C" int wcache_oldest(int blk, j_extent *extents, int max, int *n);
int wcache_oldest(int blk, j_extent *extents, int max, int *p_n)
{
    std::vector<j_extent> exts;
    int next_blk = wcache->do_get_oldest(blk, exts);
    int n = std::min(max, (int)exts.size());
    memcpy((void*)extents, exts.data(), n*sizeof(j_extent));
    *p_n = n;
    return next_blk;
}

extern "C" void wcache_write_ckpt(void);
void wcache_write_ckpt(void)
{
    wcache->do_write_checkpoint();
}

extern "C" void wcache_reset(void);
void wcache_reset(void)
{
    wcache->reset();
}

extern "C" void c_shutdown(void);
void c_shutdown(void)
{
    lsvd->shutdown();
    delete lsvd;
    delete omap;
    delete io;
}

extern "C" int c_flush(void);
int c_flush(void)
{
    return lsvd->flush();
}

extern "C" ssize_t c_init(char* name, int n, bool flushthread, bool nocache);
ssize_t c_init(char *name, int n, bool flushthread, bool nocache)
{
    io = new file_backend(name);
    omap = new objmap();
    lsvd = new translate(io, omap);
    lsvd->nocache = nocache;
    return lsvd->init(name, n, flushthread);
}

extern "C" int c_size(void);
int c_size(void)
{
    return lsvd->mapsize();
}

extern "C" int c_read(char*, uint64_t, uint32_t);
int c_read(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->read(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int c_write(char*, uint64_t, uint32_t);
int c_write(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->write(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_inmem(int max, int *list);

int dbg_inmem(int max, int *list)
{
    return lsvd->inmem(max, list);
}

int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max) 
	s->t[s->i++] = (tuple){base, limit, obj, offset, 0};
    return s->i < s->max;
}
   
extern "C" int dbg_getmap(int, int, int, struct tuple*);
int dbg_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    lsvd->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}

extern "C" int dbg_checkpoint(void);
int dbg_checkpoint(void)
{
    return lsvd->checkpoint();
}

extern "C" void dbg_reset(void);
void dbg_reset(void)
{
    lsvd->reset();
}

extern "C" int dbg_frontier(void);
int dbg_frontier(void)
{
    return lsvd->frontier();
}

extern "C" void rcache_init(uint32_t blkno, int fd);
void rcache_init(uint32_t blkno, int fd)
{
    rcache = new read_cache(blkno, fd, false, lsvd, omap, io);
}

extern "C" void rcache_shutdown(void);
void rcache_shutdown(void)
{
    delete rcache;
    rcache = NULL;
}

extern "C" void rcache_evict(int n);
void rcache_evict(int n)
{
    rcache->do_evict(n);
}

extern "C" void rcache_add(int object, int sector_offset, char* buf, size_t len);
void rcache_add(int object, int sector_offset, char* buf, size_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    memcpy(buf2, buf, len);
    extmap::obj_offset oo = {object, sector_offset};
    rcache->add(oo, len/512, buf2);
    free(buf2);
}

extern "C" void rcache_read(char *buf, uint64_t offset, uint64_t len);
void rcache_read(char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    rcache->read(offset, len, buf2);
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" void rcache_getsuper(j_read_super *p_super)
{
    j_read_super *p;
    rcache->get_info(&p, NULL, NULL, NULL, NULL);
    *p_super = *p;
}

extern "C" int rcache_getmap(extmap::obj_offset *keys, int *vals, int n);
int rcache_getmap(extmap::obj_offset *keys, int *vals, int n)
{
    int i = 0;
    std::map<extmap::obj_offset,int> *p_map;
    rcache->get_info(NULL, NULL, NULL, NULL, &p_map);
    for (auto it = p_map->begin(); it != p_map->end() && i < n; it++, i++) {
	auto [key, val] = *it;
	keys[i] = key;
	vals[i] = val;
    }
    return i;
}

extern "C" int rcache_get_flat(extmap::obj_offset *vals, int n);
int rcache_get_flat(extmap::obj_offset *vals, int n)
{
    extmap::obj_offset *p;
    j_read_super *p_super;
    rcache->get_info(&p_super, &p, NULL, NULL, NULL);
    n = std::min(n, p_super->units);
    memcpy(vals, p, n*sizeof(extmap::obj_offset));
    return n;
}

extern "C" int rcache_get_masks(uint16_t *vals, int n);
int rcache_get_masks(uint16_t *vals, int n)
{
    j_read_super *p_super;
    uint16_t *masks;
    rcache->get_info(&p_super, NULL, &masks, NULL, NULL);
    n = std::min(n, p_super->units);
    memcpy(vals, masks, n*sizeof(uint16_t));
    return n;
}

extern "C" void rcache_reset(void);
void rcache_reset(void)
{
}

extern "C" void fakemap_update(int base, int limit, int obj, int offset);
void fakemap_update(int base, int limit, int obj, int offset)
{
    extmap::obj_offset oo = {obj,offset};
    omap->map.update(base, limit, oo);
}

extern "C" void fakemap_reset(void);
void fakemap_reset(void)
{
    omap->map.reset();
}

#include "fake_rbd.h"

struct fake_rbd_image {
    backend     *io;
    objmap      *omap;
    translate   *lsvd;
    write_cache *wcache;
    read_cache  *rcache;
    ssize_t      size;		// bytes
    int          fd;		// cache device
    j_super     *js;		// cache page 0
};


struct lsvd_completion {
    rbd_callback_t cb;
    void *arg;
    int retval;
};

int rbd_aio_create_completion(void *cb_arg, rbd_callback_t complete_cb, rbd_completion_t *c)
{
    lsvd_completion *p = (lsvd_completion*)calloc(sizeof(*c), 1);
    p->cb = complete_cb;
    p->arg = cb_arg;
    *c = (rbd_completion_t)p;
    return 0;
}

int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->cb(c, p->arg);
    return 0;
}

int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->cb(c, p->arg);
    return 0;
}

void *rbd_aio_get_arg(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->arg;
}

ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->retval;
}

int rbd_aio_read(rbd_image_t image, uint64_t off, size_t len, char *buf, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    lsvd_completion *p = (lsvd_completion *)c;
    char *aligned_buf = buf;
    if (!aligned(buf, 512)) {
	aligned_buf = (char*)aligned_alloc(512, len);
	memcpy(aligned_buf, buf, len);
    }

    std::vector<write_cache::cache_miss> misses;
    iovec iov = {aligned_buf, len};
    fri->wcache->readv(off, &iov, 1, misses);

    for (auto [_off, _len, buf_offset] : misses)
	fri->rcache->read(_off, _len, aligned_buf + buf_offset);
    
    if (aligned_buf != buf) {
	memcpy(buf, aligned_buf, len);
	free(aligned_buf);
    }
    p->cb(c, p->arg);
    return 0;
}

void rbd_aio_release(rbd_completion_t c)
{
    free((void*)c);
}

void fake_rbd_cb(void *ptr)
{
    lsvd_completion *p = (lsvd_completion *)ptr;
    p->cb(ptr, p->arg);
}

int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf,
		  rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    iovec iov = {(void*)buf, len};
    fri->wcache->write(off, &iov, 1, fake_rbd_cb, (void*)c);
    return 0;
}

int rbd_close(rbd_image_t image)
{
    return 0;
}

int rbd_stat(rbd_image_t image, rbd_image_info_t *info, size_t infosize)
{
    return 0;
}

std::pair<std::string,std::string> split_string(std::string s, std::string delim)
{
    auto i = s.find(delim);
    return std::pair(s.substr(0,i), s.substr(i+delim.length()));
}

int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image, const char *snap_name)
{
    int rv;
    
    auto [nvme, obj] = split_string(std::string(name), ":");
    auto fri = new fake_rbd_image;

    // c_init:
    fri->io = new file_backend(name);
    fri->omap = new objmap();
    fri->lsvd = new translate(fri->io, fri->omap);
    fri->size = fri->lsvd->init(obj.c_str(), 2, true);

    int fd = fri->fd = open(nvme.c_str(), O_RDWR | O_DIRECT);
    j_super *js = fri->js = (j_super*)aligned_alloc(512, 4096);
    if ((rv = pread(fd, (char*)js, 4096, 0)) < 0)
	return rv;
    if (js->magic != LSVD_MAGIC || js->type != LSVD_J_SUPER)
	return -1;
    
    fri->wcache = new write_cache(js->write_super, fd, fri->lsvd);
    fri->rcache = new read_cache(js->read_super, fd, false, fri->lsvd, fri->omap, fri->io);

    *image = (void*)fri;
    return 0;
}

fake_rbd_image _fri;

extern "C" void fake_rbd_init(void);
void fake_rbd_init(void)
{
    _fri.io = io;
    _fri.omap = omap;
    _fri.lsvd = lsvd;
    _fri.size = 0;
    _fri.wcache = wcache;
    _fri.rcache = rcache;
}

void fake_rbd_cb(rbd_completion_t c, void *arg)
{
    do_write *d = (do_write*)arg;
    std::unique_lock lk(d->m);
    d->done = true;
    d->cv.notify_all();
}

extern "C" void fake_rbd_read(char *buf, size_t off, size_t len);
void fake_rbd_read(char *buf, size_t off, size_t len)
{
    rbd_completion_t c;
    do_write dw;
    rbd_aio_create_completion((void*)&dw, fake_rbd_cb, &c);
    rbd_aio_read((rbd_image_t)&_fri, off, len, buf, c);
    std::unique_lock lk(dw.m);
    while (!dw.done)
	dw.cv.wait(lk);
    rbd_aio_release(c);
}

extern "C" void fake_rbd_write(char *buf, size_t off, size_t len);
void fake_rbd_write(char *buf, size_t off, size_t len)
{
    rbd_completion_t c;
    do_write dw;
    rbd_aio_create_completion((void*)&dw, fake_rbd_cb, &c);
    rbd_aio_write((rbd_image_t)&_fri, off, len, buf, c);
    std::unique_lock lk(dw.m);
    while (!dw.done)
	dw.cv.wait(lk);
    rbd_aio_release(c);
}

/* any following functions are stubs only
 */
int rbd_invalidate_cache(rbd_image_t image)
{
    return 0;
}

int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps, int numcomp)
{
    return 0;
}

int rbd_set_image_notification(rbd_image_t image, int fd, int type)
{
    return 0;
}

/* we just need null implementations of the RADOS functions.
 */
int rados_conf_read_file(rados_t cluster, const char *path)
{
    return 0;
}

int rados_conf_set(rados_t cluster, const char *option, const char *value)
{
    return 0;
}

int rados_connect(rados_t cluster)
{
    return 0;
}

int rados_create(rados_t *cluster, const char * const id)
{
    return 0;
}

int rados_create2(rados_t *pcluster, const char *const clustername,
                  const char * const name, uint64_t flags)
{
    return 0;
}

int rados_ioctx_create(rados_t cluster, const char *pool_name, rados_ioctx_t *ioctx)
{
    return 0;
}

void rados_ioctx_destroy(rados_ioctx_t io)
{
}

void rados_shutdown(rados_t cluster)
{
}
