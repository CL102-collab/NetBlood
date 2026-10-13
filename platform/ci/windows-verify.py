#!/usr/bin/env python3
"""Reject wrong-architecture, incomplete, or non-baseline Windows packages.

PE checks run without executing Windows code. The XP import denylist catches
known post-XP APIs and unsupported CRTs; it complements, rather than replaces,
an actual Windows XP runtime test. The no-SSE2 check disassembles all executable
sections, including statically linked third-party code and compiler runtimes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

MACHINES = {"windows-x86_64": 0x8664, "windows-i686": 0x14c,
            "windows-xp-i686-nosse2": 0x14c, "windows-arm64": 0xaa64}
SYSTEM_DLLS = set("advapi32 avrt bcrypt cfgmgr32 comctl32 comdlg32 crypt32 d3d9 d3d11 d3d12 dbghelp "
                 "dinput dinput8 dnsapi dsound dwmapi dxgi gdi32 hid imm32 iphlpapi kernel32 mpr msvcrt "
                 "mswsock ntdll ole32 oleaut32 powrprof psapi rpcrt4 secur32 setupapi shell32 shlwapi "
                 "user32 userenv usp10 uxtheme version winhttp wininet winmm winspool wintrust ws2_32 "
                 "wsock32 wtsapi32 ucrtbase".split())
XP_DLLS = SYSTEM_DLLS - set("avrt bcrypt d3d11 d3d12 dwmapi dxgi ucrtbase".split())
XP_FORBIDDEN_APIS = set("AcquireSRWLockExclusive AcquireSRWLockShared AddDllDirectory CancelIoEx CompareStringEx "
                       "CreateEventExA CreateEventExW CreateFile2 CreateMutexExA CreateMutexExW "
                       "CreateSemaphoreExA CreateSemaphoreExW CreateWaitableTimerExA CreateWaitableTimerExW "
                       "EnumSystemLocalesEx FlsAlloc FlsFree FlsGetValue "
                       "FlsSetValue GetActiveProcessorCount GetActiveProcessorGroupCount GetMaximumProcessorCount "
                       "GetCurrentProcessorNumber GetCurrentProcessorNumberEx "
                       "GetDynamicTimeZoneInformation GetFileInformationByHandleEx GetFinalPathNameByHandleA "
                       "GetFinalPathNameByHandleW GetLocaleInfoEx GetLogicalProcessorInformationEx "
                       "GetThreadId QueryFullProcessImageNameA QueryFullProcessImageNameW "
                       "GetSystemTimePreciseAsFileTime GetTickCount64 GetUserDefaultLocaleName "
                       "GetThreadGroupAffinity GetThreadInformation SetThreadInformation "
                       "QueryUnbiasedInterruptTime QueryUnbiasedInterruptTimePrecise QueryInterruptTime QueryInterruptTimePrecise "
                       "InitializeConditionVariable InitializeCriticalSectionEx InitializeSRWLock "
                       "InitOnceBeginInitialize InitOnceComplete InitOnceExecuteOnce InitOnceInitialize "
                       "InetNtopA InetNtopW InetPtonA InetPtonW LCMapStringEx RegGetValueA RegGetValueW "
                       "ReleaseSRWLockExclusive ReleaseSRWLockShared RemoveDllDirectory SetDefaultDllDirectories "
                       "SetThreadDescription SetWaitableTimerEx WSAPoll "
                       "SetThreadErrorMode SleepConditionVariableCS SleepConditionVariableSRW "
                       "TryAcquireSRWLockExclusive TryAcquireSRWLockShared WakeAllConditionVariable "
                       "WakeByAddressAll WakeByAddressSingle WakeConditionVariable WaitOnAddress".split())
# XMM instructions introduced by the original SSE (Pentium III) are allowed.
SSE1 = set("addps addss andnps andps cmpps cmpss comiss cvtpi2ps cvtps2pi cvtsi2ss cvtss2si "
           "cvttps2pi cvttss2si divps divss ldmxcsr maxps maxss minps minss movaps movhps movlhps "
           "movhlps movlps movmskps movntps movss movups mulps mulss orps rcpps rcpss rsqrtps "
           "rsqrtss shufps sqrtps sqrtss stmxcsr subps subss ucomiss unpckhps unpcklps xorps".split())
# SSE2/SSSE3 also added instructions using MMX registers: looking for XMM alone
# would miss those. Include later scalar instructions compilers/runtimes may emit.
# Do not reject TZCNT's backward-compatible REP-BSF encoding: GCC emits it for
# __builtin_ctz on i686, where the zero-input result is intentionally undefined.
# Older CPUs execute BSF with the same nonzero-input result. LZCNT is different:
# its fallback BSR result differs even for nonzero operands, so it stays banned.
POST_PENTIUM3 = set("paddq psubq pmuludq pabsb pabsw pabsd palignr phaddw phaddd phaddsw "
                   "phsubw phsubd phsubsw pmaddubsw pmulhrsw pshufb psignb psignw psignd "
                   "lfence mfence movnti movntil movntiq clflush clflushopt clwb monitor mwait "
                   "popcnt popcntl popcntw popcntq lzcnt lzcntl lzcntw lzcntq "
                   "andn bextr blsi blsmsk blsr bzhi mulx pdep pext "
                   "rorx sarx shlx shrx adcx adox rdrand rdseed rdtscp movbe prefetchw prefetchwt1".split())


def pe_info(path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path} is not a PE executable")
    pe = struct.unpack_from("<I", data, 60)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path} has no PE header")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic not in (0x10b, 0x20b):
        raise ValueError(f"Unsupported PE format {magic:#x}")
    sections = []
    for index in range(count):
        entry = optional + optional_size + index * 40
        size, address, raw_size, raw = struct.unpack_from("<IIII", data, entry + 8)
        sections.append((address, max(size, raw_size), raw))

    def offset(address):
        for start, length, raw in sections:
            if start <= address < start + length:
                return raw + address - start
        raise ValueError(f"Unmapped PE address {address:#x} in {path}")

    def string(address):
        start = offset(address)
        return data[start:data.index(b"\0", start)].decode("ascii")

    imports = {}
    directory = optional + (112 if magic == 0x20b else 96)
    import_rva = struct.unpack_from("<I", data, directory + 8)[0]
    if import_rva:
        descriptor = offset(import_rva)
        while True:
            lookup, _, _, name, thunk = struct.unpack_from("<IIIII", data, descriptor)
            if not any((lookup, name, thunk)):
                break
            dll = string(name).lower()
            imported = []
            entry = offset(lookup or thunk)
            width, fmt = (8, "<Q") if magic == 0x20b else (4, "<I")
            while value := struct.unpack_from(fmt, data, entry)[0]:
                if not value & (1 << (width * 8 - 1)):
                    imported.append(string(value + 2))
                else:
                    imported.append("ordinal:" + str(value & 0xffff))
                entry += width
            imports[dll] = imported
            descriptor += 20
    return {"machine": machine,
            "os_version": list(struct.unpack_from("<HH", data, optional + 40)),
            "subsystem_version": list(struct.unpack_from("<HH", data, optional + 48)),
            "imports": imports}


def cpp_entropy_dispatch(functions):
    """Recognize GCC's three scalar entropy helpers and their CPUID selector.

    MinGW libstdc++ contains these optional instructions even in i686 builds.
    GCC's random.cc selects them only after CPUID.01H:ECX[30] (RDRAND) and
    CPUID.07H:EBX[18] (RDSEED). Require the actual selector's feature probes,
    conditional selection, and references to all helpers in this executable;
    names alone are not an exemption. Unknown compiler layouts fail closed.
    """
    helpers = {}
    for name, block in functions.items():
        for token in ("12__x86_rdrandEPv", "12__x86_rdseedEPv", "19__x86_rdseed_rdrandEPv"):
            if (name.endswith("_ZNSt12_GLOBAL__N_1" + token)
                    and any(item[1] in {"rdrand", "rdseed"} for item in block)):
                helpers[name] = block
    if len(helpers) != 3:
        return None
    for name, block in functions.items():
        if "ZNSt13random_device7_M_initERKNSt7__cxx11" not in name:
            continue
        instructions = [item[1:] for item in block]
        gates = []
        for index, (mnemonic, operands, line) in enumerate(instructions):
            if mnemonic not in {"and", "test", "andl", "testl"}:
                continue
            if operands not in {"$0x40000000,%ecx", "$0x40000,%ebx"}:
                continue
            before, after = instructions[max(0, index - 6):index], instructions[index + 1:index + 4]
            if (any(item[0] == "cpuid" for item in before)
                    and any(item[0] in {"je", "jz", "jne", "jnz", "cmove", "cmovne"} for item in after)):
                gates.append((operands, line))
        text = "\n".join(item[3] for item in block)
        if (set(item[0] for item in gates) != {"$0x40000000,%ecx", "$0x40000,%ebx"}
                or not re.search(r"\bmov\s+\$0x1,%eax", text)
                or not re.search(r"\bmov\s+\$0x7,%eax", text)
                or any(not re.search(r"\$0x" + format(items[0][0], "x") + r"\b", text)
                       for items in helpers.values())):
            continue
        return {"selector": name, "feature_checks": [item[1] for item in gates],
                "helpers": sorted(helpers),
                "source": "GCC libstdc++-v3/src/c++11/random.cc (CPUID-dispatched scalar entropy)"}
    return None


def verify_no_sse2(path, objdump, allow_cpp_entropy=False):
    listing = subprocess.check_output([objdump, "-d", "--no-show-raw-insn", str(path)], text=True)
    functions = {}
    current = ""
    for line in listing.splitlines():
        if label := re.match(r"^([0-9a-f]+) <(.+)>:$", line):
            current = label[2]
            functions[current] = []
        if instruction := re.match(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)$", line):
            functions.setdefault(current, []).append((int(instruction[1], 16), instruction[2],
                                                       instruction[3], line.strip()))
    entropy = cpp_entropy_dispatch(functions) if allow_cpp_entropy else None
    unsupported = []
    allowed = []
    current = ""
    for line in listing.splitlines():
        if label := re.match(r"^[0-9a-f]+ <(.+)>:$", line):
            current = label[1]
        instruction = re.match(r"^\s*[0-9a-f]+:\s+([a-z][a-z0-9.]*)\s*(.*)$", line)
        if not instruction:
            continue
        mnemonic, operands = instruction.groups()
        if mnemonic in {"rdrand", "rdseed"} and entropy and current in entropy["helpers"]:
            allowed.append({"function": current, "instruction": line.strip()})
            continue
        # objdump spells SSE comparisons using either generic or predicate names.
        sse1_compare = re.fullmatch(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ps|ss)", mnemonic)
        if (re.search(r"[%\b](ymm|zmm)\d", operands)
                or ("xmm" in operands and mnemonic not in SSE1 and not sse1_compare)
                or (mnemonic.startswith("v") and mnemonic not in {"verr", "verw"})
                or mnemonic in POST_PENTIUM3 or mnemonic.startswith("fisttp")):
            unsupported.append(line.strip())
    if unsupported:
        raise ValueError(f"{path.name} contains instructions newer than Pentium III:\n" + "\n".join(unsupported[:30]))
    return {"no_sse2": True, "audited_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "optional_scalar_entropy": {**entropy, "instructions": allowed} if allowed else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--target", choices=MACHINES, required=True)
    parser.add_argument("--objdump", required=True)
    args = parser.parse_args()
    xp = args.target == "windows-xp-i686-nosse2"
    files = {path.name.lower(): path for path in args.directory.iterdir() if path.suffix.lower() in (".exe", ".dll")}
    if "nblood.exe" not in files:
        raise ValueError("Package is missing nblood.exe")
    report = {}
    for name, path in files.items():
        info = pe_info(path)
        if info["machine"] != MACHINES[args.target]:
            raise ValueError(f"Wrong PE architecture in {name}: {info['machine']:#x}")
        if xp and (info["subsystem_version"] > [5, 1] or info["os_version"] > [5, 1]):
            raise ValueError(f"{name} requires an OS newer than Windows XP")
        for dll, imports in info["imports"].items():
            system = dll.removesuffix(".dll") in (XP_DLLS if xp else SYSTEM_DLLS)
            system |= not xp and dll.startswith(("api-ms-win-", "ext-ms-win-"))
            if not system and dll not in files:
                raise ValueError(f"{name} requires an unpackaged runtime DLL: {dll}")
            if xp and (unsupported := XP_FORBIDDEN_APIS.intersection(imports)):
                raise ValueError(f"{name} imports post-XP APIs: {sorted(unsupported)}")
        if xp:
            info["cpu_audit"] = verify_no_sse2(path, args.objdump, allow_cpp_entropy=True)
        report[name] = info
    (args.directory / "windows-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified {args.target}: {len(files)} PE files, target architecture and runtime closure"
          + (", XP API/subsystem and no-SSE2 checks" if xp else ""))


if __name__ == "__main__":
    main()
