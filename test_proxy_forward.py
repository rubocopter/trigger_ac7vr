import ctypes

PROXY = r"E:\trigger_ac7vr\build\Release\xinput1_3.dll"
ERROR_SUCCESS = 0
ERROR_DEVICE_NOT_CONNECTED = 1167

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
k32.LoadLibraryW.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
k32.GetProcAddress.restype = ctypes.c_void_p

module = k32.LoadLibraryW(PROXY)
assert module, f"LoadLibraryW failed: {ctypes.get_last_error()}"

for ordinal, size in ((2, 16), (3, 4)):
    address = k32.GetProcAddress(module, ctypes.c_void_p(ordinal))
    assert address, f"ordinal {ordinal} is missing"

    function_type = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p)
    function = function_type(address)
    buffer = ctypes.create_string_buffer(size)
    result = function(0, ctypes.cast(buffer, ctypes.c_void_p))

    assert result in (ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED), (
        f"ordinal {ordinal} returned unexpected code {result}"
    )
    print(f"ordinal {ordinal}: {result}")
