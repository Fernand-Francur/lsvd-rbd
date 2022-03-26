from ctypes import *

class tuple(Structure):
    _fields_ = [("base",   c_int),
                ("limit",  c_int),
                ("obj",    c_int),
                ("offset", c_int),
                ("plba",   c_int)]

LSVD_SUPER = 1
LSVD_DATA = 2
LSVD_CKPT = 3
LSVD_MAGIC = 0x4456534c

# these match version 453d93 of objects.cc

class hdr(Structure):
    _fields_ = [("magic",               c_uint),
                ("version",             c_uint),
                ("vol_uuid",            c_ubyte*16),
                ("type",                c_uint),
                ("seq",                 c_uint),
                ("hdr_sectors",         c_uint),
                ("data_sectors",        c_uint)]
sizeof_hdr = sizeof(hdr) # 40

class super_hdr(Structure):
    _fields_ = [("vol_size",            c_ulong),
                ("total_sectors",       c_ulong),
                ("live_sectors",        c_ulong),
                ("next_obj",            c_uint),
                ("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("clones_offset",       c_uint),
                ("clones_len",          c_uint),
                ("snaps_offset",        c_uint),
                ("snaps_len",           c_uint)]
sizeof_super_hdr = sizeof(super_hdr) # 56

# ckpts is array of uint32

class clone(Structure):
    _pack_ = 1
    _fields_ = [("vol_uuid",            c_ubyte*16),
                ("sequence",            c_uint),
                ("name_len",            c_ubyte)]   # 21 bytes
        # followed by 'name_len' bytes of name, use string_at
sizeof_clone = sizeof(clone) # 21

class snap(Structure):
    _fields_ = [("snap_uuid",           c_ubyte*16),
                ("seq",                 c_uint)]
sizeof_snap = sizeof(snap) # 20

class data_hdr(Structure):
    _fields_ = [("last_data_obj",       c_uint),
                ("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("objs_cleaned_offset", c_uint),
                ("objs_cleaned_len",    c_uint),
                ("map_offset",          c_uint),
                ("map_len",             c_uint)]
sizeof_data_hdr = sizeof(data_hdr) # 28

class obj_cleaned(Structure):
    _fields_ = [("seq",                 c_uint),
                ("was_deleted",         c_uint)]
sizeof_obj_cleaned = sizeof(obj_cleaned) # 8

class data_map(LittleEndianStructure):
    _fields_ = [("lba",                 c_ulong, 36),
                ("len",                 c_ulong, 28)]
sizeof_data_map = sizeof(data_map) # 8

class ckpt_hdr(Structure):
    _fields_ = [("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("objs_offset",         c_uint),
                ("objs_len",            c_uint),
                ("deletes_offset",      c_uint),
                ("deletes_len",         c_uint),
                ("map_offset",          c_uint),
                ("map_len",             c_uint)]
sizeof_ckpt_hdr = sizeof(ckpt_hdr) # 32

class ckpt_obj(Structure):
    _fields_ = [("seq",                 c_uint),
                ("hdr_sectors",         c_uint),
                ("data_sectors",        c_uint),
                ("live_sectors",        c_uint)]
sizeof_ckpt_obj = sizeof(ckpt_obj) # 16

class deferred_delete(Structure):
    _fields_ = [("seq",                 c_uint),
                ("time",                c_uint)]
sizeof_deferred_delete = sizeof(deferred_delete)

class ckpt_mapentry(LittleEndianStructure):
    _fields_ = [("lba",                 c_ulong, 36),
                ("len",                 c_ulong, 28),
                ("obj",                 c_uint),
                ("offset",              c_uint)]
sizeof_ckpt_mapentry = sizeof(ckpt_mapentry) # 16

class obj_offset(LittleEndianStructure):
    _fields_ = [("obj", c_ulong, 36),
                ("offset", c_ulong, 28)]
sizeof_obj_offset = sizeof(obj_offset)

rsuper = None
def rcache_init(blkno):
    global rsuper
    assert _fd != -1
    lsvd_lib.rcache_init(c_uint(blkno), c_int(_fd))
    rsuper = j_read_super()
    lsvd_lib.rcache_getsuper(byref(rsuper))

def rcache_shutdown():
    lsvd_lib.rcache_shutdown()

def rcache_read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * nbytes)()
    lsvd_lib.rcache_read(buf, c_ulong(offset), c_ulong(nbytes))
    return buf[0:nbytes]

def rcache_add(obj, offset, data):
    nbytes = len(data)
    assert (nbytes % 512) == 0
    lsvd_lib.rcache_add(c_int(obj), c_int(offset), data, c_int(nbytes), data)
    
def rcache_getmap():
    n = rsuper.units
    k = (obj_offset * n)()
    v = (c_int * n)()
    m = lsvd_lib.rcache_getmap(byref(k), byref(v), n)
    keys = [[_.obj, _.offset] for _ in k[0:m]]
    vals = v[:]
    return list(zip(keys, vals))
    
def rcache_flatmap():
    n = rsuper.units
    vals = (obj_offset * n)()
    n = lsvd_lib.rcache_get_flat(byref(vals), c_int(n))
    return [[_.obj,_.offset] for _ in vals[0:n]]

def rcache_bitmap():
    n = rsuper.units
    vals = (c_ushort * n)()
    n = lsvd_lib.rcache_get_masks(byref(vals), c_int(n))
    return vals[0:n]

def rcache_evict(n):
    lsvd_lib.rcache_evict(c_int(n))

def rcache_reset():
    lsvd_lib.rcache_reset()

class j_extent(LittleEndianStructure):
    _fields_ = [("lba", c_ulong, 40),
                ("len", c_ulong, 24)]
sizeof_j_extent = sizeof(j_extent)

class j_map_extent(LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("lba",  c_ulong, 40),
                ("len",  c_ulong, 24),
                ("plba", c_ulong)]
sizeof_j_map_extent = sizeof(j_map_extent)

class j_length(Structure):
    _fields_ = [("page", c_uint),
                ("len", c_uint)]
sizeof_j_length = sizeof(j_length)

LSVD_J_DATA    = 10
LSVD_J_CKPT    = 11
LSVD_J_PAD     = 12
LSVD_J_SUPER   = 13
LSVD_J_W_SUPER = 14
LSVD_J_R_SUPER = 15

class j_hdr(Structure):
    _fields_ = [("magic",         c_uint),
                ("type",          c_uint),
                ("version",       c_uint),
                ("vol_uuid",      c_ubyte*16),
                ("seq",           c_ulong),
                ("len",           c_uint),
                ("crc32",         c_uint),
                ("extent_offset", c_uint),
                ("extent_len",    c_uint)]
sizeof_j_hdr = sizeof(j_hdr)

class j_write_super(Structure):
    _fields_ = [("magic",       c_uint),
                ("type",        c_uint),
                ("version",     c_uint),
                ("vol_uuid",    c_ubyte*16),
                ("seq",         c_ulong),
                ("meta_base",   c_uint),
                ("meta_limit",  c_uint),
                ("base",        c_uint),
                ("limit",       c_uint),
                ("next",        c_uint),
                ("oldest",      c_uint),
                ("map_start",   c_uint),
                ("map_blocks",  c_uint),
                ("map_entries", c_uint),
                ("len_start",   c_uint),
                ("len_blocks",  c_uint),
                ("len_entries", c_uint)]
sizeof_j_write_super = sizeof(j_write_super)

class j_read_super(Structure):
    _fields_ = [("magic",        c_uint),
                ("type",         c_uint),
                ("version",      c_uint),
                ("vol_uuid",     c_ubyte*16),
                ("unit_size",    c_int),
                ("base",         c_int),
                ("units",        c_int),
                ("map_start",    c_int),
                ("map_blocks",   c_int),
                ("bitmap_start", c_int),
                ("bitmap_blocks", c_int),
                ("evict_type",   c_int),
                ("evict_start",  c_int),
                ("evict_blocks", c_int)]
sizeof_j_read_super = sizeof(j_read_super)

class j_super(Structure):
    _fields_ = [("magic",        c_uint),
                ("type",         c_uint),
                ("version",      c_uint),
                ("write_super",  c_uint),
                ("read_super",   c_uint),
                ("vol_uuid",     c_ubyte*16),
                ("backend_type", c_uint)]
sizeof_j_super = sizeof(j_super)

LSVD_BE_FILE  = 20
LSVD_BE_S3    = 21
LSVD_BE_RADOS = 22

class j_be_file(Structure):
    _fields_ = [("len", c_ushort),
                ("prefix", c_char*24)]      # hack. max of 24 bytes
