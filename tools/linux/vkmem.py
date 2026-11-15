#!/usr/bin/env python3
"""Print the Vulkan memory limits of every device the loader finds (no vulkaninfo needed):
heaps, maxMemoryAllocationSize, maxBufferSize, maxStorageBufferRange. Use VK_DRIVER_FILES to pick an ICD."""
import ctypes as C, struct

vk = C.CDLL("libvulkan.so.1")
app = (C.c_char * 48)()
struct.pack_into("<IQQIQIQI", app, 0, 0, 0, 0, 0, 0, 0, 0, (1 << 22) | (3 << 12))  # sType 0 placeholder, fixed below
# VkApplicationInfo: sType(4)pad(4) pNext(8) pApplicationName(8) applicationVersion(4)pad pEngineName(8) engineVersion(4) apiVersion(4)
app = (C.c_char * 48)()
struct.pack_into("<I", app, 0, 0)          # VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
struct.pack_into("<I", app, 44, (1 << 22) | (3 << 12))
ici = (C.c_char * 64)()
struct.pack_into("<I", ici, 0, 1)          # INSTANCE_CREATE_INFO
struct.pack_into("<Q", ici, 24, C.addressof(app))
inst = C.c_void_p()
r = vk.vkCreateInstance(ici, None, C.byref(inst))
print("vkCreateInstance", r)
n = C.c_uint32(0)
vk.vkEnumeratePhysicalDevices(inst, C.byref(n), None)
devs = (C.c_void_p * n.value)()
vk.vkEnumeratePhysicalDevices(inst, C.byref(n), devs)
for d in devs:
    p2 = (C.c_char * 1024)(); m3 = (C.c_char * 64)(); m4 = (C.c_char * 64)()
    struct.pack_into("<I", p2, 0, 1000059001)          # PROPERTIES_2
    struct.pack_into("<Q", p2, 8, C.addressof(m3))
    struct.pack_into("<I", m3, 0, 1000168000)          # MAINTENANCE_3_PROPERTIES
    struct.pack_into("<Q", m3, 8, C.addressof(m4))
    struct.pack_into("<I", m4, 0, 1000413001)          # MAINTENANCE_4_PROPERTIES
    vk.vkGetPhysicalDeviceProperties2(d, p2)
    name = bytes(p2[16 + 20:16 + 276]).split(b"\0")[0].decode()
    api = struct.unpack_from("<I", p2, 16)[0]
    stor = struct.unpack_from("<I", p2, 16 + 296 + 28)[0]
    maxalloc = struct.unpack_from("<Q", m3, 24)[0]
    maxbuf = struct.unpack_from("<Q", m4, 16)[0]
    mem = (C.c_char * 520)()
    vk.vkGetPhysicalDeviceMemoryProperties(d, mem)
    ntypes = struct.unpack_from("<I", mem, 0)[0]
    nheaps = struct.unpack_from("<I", mem, 260)[0]
    print("device %s api %d.%d.%d" % (name, api >> 22, (api >> 12) & 1023, api & 4095))
    print("  maxMemoryAllocationSize %d MiB (%d B), maxBufferSize %d MiB (%d B), maxStorageBufferRange %d MiB" % (maxalloc >> 20, maxalloc, maxbuf >> 20, maxbuf, stor >> 20))
    for i in range(nheaps):
        size, flags = struct.unpack_from("<QI", mem, 264 + 16 * i)
        print("  heap %d: %d MiB flags 0x%x" % (i, size >> 20, flags))
    for i in range(ntypes):
        fl, h = struct.unpack_from("<II", mem, 4 + 8 * i)
        print("  type %d: heap %d flags 0x%x" % (i, h, fl))


# createBuffer size probe (sparse residency buffers, as the engine's 4 GiB arena pages): sizes in MiB from argv
import sys
if len(sys.argv) > 1:
    d = devs[0]
    feats = (C.c_uint32 * 55)()
    feats[44] = 1; feats[45] = 1
    prio = C.c_float(1.0)
    qci = (C.c_char * 40)()
    struct.pack_into("<IQIIIQ", qci, 0, 2, 0, 0, 0, 1, C.addressof(prio))
    # layout: sType@0 pNext@8 flags@16 family@20 count@24 prio@32
    struct.pack_into("<I", qci, 0, 2); struct.pack_into("<Q", qci, 8, 0); struct.pack_into("<IIIQ", qci, 16, 0, 0, 1, C.addressof(prio))
    dci = (C.c_char * 72)()
    struct.pack_into("<I", dci, 0, 3); struct.pack_into("<I", dci, 20, 1); struct.pack_into("<Q", dci, 24, C.addressof(qci))
    struct.pack_into("<Q", dci, 64, C.addressof(feats))
    dev = C.c_void_p()
    print("vkCreateDevice", vk.vkCreateDevice(d, dci, None, C.byref(dev)))
    for mib in sys.argv[1:]:
        size = int(float(mib) * 1048576)
        for flags in (0, 3):
            bci = (C.c_char * 56)()
            struct.pack_into("<I", bci, 0, 12); struct.pack_into("<I", bci, 16, flags)
            struct.pack_into("<Q", bci, 24, size); struct.pack_into("<I", bci, 32, 0x23)
            buf = C.c_void_p()
            r = vk.vkCreateBuffer(dev, bci, None, C.byref(buf))
            print("createBuffer %s MiB sparse=%d -> %d (%s)" % (mib, flags != 0, r, "OK" if r == 0 else "ErrorOutOfDeviceMemory" if r == -2 else "err"))
