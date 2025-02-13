// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//----------------------------------------------------------
// SPMIUtil.cpp - General utility functions
//----------------------------------------------------------

#include "standardpch.h"
#include "logging.h"
#include "spmiutil.h"

#include <minipal/debugger.h>
#include <minipal/random.h>

static bool breakOnDebugBreakorAV = false;

bool BreakOnDebugBreakorAV()
{
    return breakOnDebugBreakorAV;
}

void SetBreakOnDebugBreakOrAV(bool value)
{
    breakOnDebugBreakorAV = value;
}

static bool breakOnException = false;

bool BreakOnException()
{
    return breakOnException;
}

void SetBreakOnException(bool value)
{
    breakOnException = value;
}

void DebugBreakorAV(int val)
{
    if (minipal_is_native_debugger_present())
    {
        if (val == 0)
            DEBUG_BREAK;
        if (BreakOnDebugBreakorAV())
            DEBUG_BREAK;
    }

    int exception_code = EXCEPTIONCODE_DebugBreakorAV + val;
    // assert((EXCEPTIONCODE_DebugBreakorAV <= exception_code) && (exception_code < EXCEPTIONCODE_DebugBreakorAV_MAX))
    LogException(exception_code, "DebugBreak or AV Exception %d", val);
}

char* GetEnvironmentVariableWithDefaultA(const char* envVarName, const char* defaultValue)
{
    char* retString = nullptr;

    // Figure out how much space we need to allocate
    DWORD dwRetVal = ::GetEnvironmentVariableA(envVarName, nullptr, 0);
    if (dwRetVal != 0)
    {
        retString = new char[dwRetVal];
        dwRetVal  = ::GetEnvironmentVariableA(envVarName, retString, dwRetVal);
    }
    else
    {
        if (defaultValue != nullptr)
        {
            dwRetVal  = (DWORD)strlen(defaultValue) + 1; // add one for null terminator
            retString = new char[dwRetVal];
            memcpy_s(retString, dwRetVal, defaultValue, dwRetVal);
        }
    }

    return retString;
}

WCHAR* GetEnvironmentVariableWithDefaultW(const WCHAR* envVarName, const WCHAR* defaultValue)
{
    WCHAR* retString = nullptr;

    // Figure out how much space we need to allocate
    DWORD dwRetVal = ::GetEnvironmentVariableW(envVarName, nullptr, 0);
    if (dwRetVal != 0)
    {
        retString = new WCHAR[dwRetVal];
        dwRetVal  = ::GetEnvironmentVariableW(envVarName, retString, dwRetVal);
    }
    else
    {
        if (defaultValue != nullptr)
        {
            dwRetVal  = (DWORD)u16_strlen(defaultValue) + 1; // add one for null terminator
            retString = new WCHAR[dwRetVal];
            memcpy_s(retString, dwRetVal * sizeof(WCHAR), defaultValue, dwRetVal * sizeof(WCHAR));
        }
    }

    return retString;
}

#ifdef TARGET_UNIX
// For some reason, the PAL doesn't have GetCommandLineA(). So write it.
LPSTR GetCommandLineA()
{
    LPSTR  pCmdLine  = nullptr;
    LPWSTR pwCmdLine = GetCommandLineW();

    if (pwCmdLine != nullptr)
    {
        // Convert to ASCII

        int n = WideCharToMultiByte(CP_ACP, 0, pwCmdLine, -1, nullptr, 0, nullptr, nullptr);
        if (n == 0)
        {
            LogError("MultiByteToWideChar failed %d", GetLastError());
            return nullptr;
        }

        pCmdLine = new char[n];

        int n2 = WideCharToMultiByte(CP_ACP, 0, pwCmdLine, -1, pCmdLine, n, nullptr, nullptr);
        if ((n2 == 0) || (n2 != n))
        {
            LogError("MultiByteToWideChar failed %d", GetLastError());
            return nullptr;
        }
    }

    return pCmdLine;
}
#endif // TARGET_UNIX

bool LoadRealJitLib(HMODULE& jitLib, WCHAR* jitLibPath)
{
    // Load Library
    if (jitLib == NULL)
    {
        if (jitLibPath == nullptr)
        {
            LogError("LoadRealJitLib - No real jit path");
            return false;
        }
        jitLib = ::LoadLibraryExW(jitLibPath, NULL, 0);
        if (jitLib == NULL)
        {
            LogError("LoadRealJitLib - LoadLibrary failed to load '%ws' (0x%08x)", jitLibPath, ::GetLastError());
            return false;
        }
    }
    return true;
}

void ReplaceIllegalCharacters(WCHAR* fileName)
{
    WCHAR* quote = nullptr;

    // Perform the following transforms:
    //  - Convert non-ASCII to ASCII for simplicity
    //  - Remove any illegal or annoying characters from the file name by
    // converting them to underscores.
    //  - Replace any quotes in the file name with spaces.
    for (quote = fileName; *quote != '\0'; quote++)
    {
        WCHAR ch = *quote;
        if ((ch <= 32) || (ch >= 127)) // Only allow textual ASCII characters
        {
            *quote = W('_');
        }
        else
        {
            switch (ch)
            {
                case W('('): case W(')'): case W('='): case W('<'):
                case W('>'): case W(':'): case W('/'): case W('\\'):
                case W('|'): case W('?'): case W('!'): case W('*'):
                case W('.'): case W(','):
                    *quote = W('_');
                    break;
                case W('"'):
                    *quote = W(' ');
                    break;
                default:
                    break;
            }
        }
    }
}

// All lengths in this function exclude the terminal NULL.
WCHAR* GetResultFileName(const WCHAR* folderPath, const WCHAR* fileName, const WCHAR* extension)
{
    const size_t extensionLength    = u16_strlen(extension);
    const size_t fileNameLength     = u16_strlen(fileName);
    const size_t randomStringLength = 8;
    const size_t maxPathLength      = MAX_PATH - 50;

    // See how long the folder part is, and start building the file path with the folder part.
    //
    WCHAR* fullPath = new WCHAR[MAX_PATH];
    fullPath[0] = W('\0');
    const size_t folderPathLength = GetFullPathNameW(folderPath, MAX_PATH, (LPWSTR)fullPath, NULL);

    if (folderPathLength == 0)
    {
        LogError("GetResultFileName - can't resolve folder path '%ws'", folderPath);
        return nullptr;
    }

    // Account for the folder, directory separator and extension.
    //
    size_t fullPathLength = folderPathLength + 1 + extensionLength;

    // If we won't have room for a minimal file name part, bail.
    //
    if ((fullPathLength + randomStringLength) > maxPathLength)
    {
        LogError("GetResultFileName - folder path '%ws' length + minimal file name exceeds limit %d", fullPath, maxPathLength);
        return nullptr;
    }

    // Now figure out the file name part.
    //
    const size_t maxFileNameLength = maxPathLength - fullPathLength;
    size_t usableFileNameLength = min(fileNameLength, maxFileNameLength - randomStringLength);
    fullPathLength += usableFileNameLength + randomStringLength;

    // Append the file name part
    //
    wcsncat_s(fullPath, fullPathLength + 1, DIRECTORY_SEPARATOR_STR_W, 1);
    wcsncat_s(fullPath, fullPathLength + 1, fileName, usableFileNameLength);

    // Clean up anything in the file part that can't be in a file name.
    //
    ReplaceIllegalCharacters(fullPath + folderPathLength + 1);

    // Append a random string to improve uniqueness.
    //
    unsigned int randomNumber = 0;
    minipal_get_non_cryptographically_secure_random_bytes((uint8_t*)&randomNumber, sizeof(randomNumber));

    WCHAR randomString[randomStringLength + 1];
    FormatInteger(randomString, randomStringLength + 1, "%08X", randomNumber);
    wcsncat_s(fullPath, fullPathLength + 1, randomString, randomStringLength);

    // Append extension
    //
    wcsncat_s(fullPath, fullPathLength + 1, extension, extensionLength);

    return fullPath;
}

#ifdef TARGET_AMD64
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_AMD64;
#elif defined(TARGET_X86)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_X86;
#elif defined(TARGET_ARM)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_ARM;
#elif defined(TARGET_ARM64)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_ARM64;
#elif defined(TARGET_LOONGARCH64)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_LOONGARCH64;
#elif defined(TARGET_RISCV64)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_RISCV64;
#elif defined(TARGET_POWERPC64)
static SPMI_TARGET_ARCHITECTURE SpmiTargetArchitecture = SPMI_TARGET_ARCHITECTURE_POWERPC64;
#else
#error Unsupported architecture
#endif

SPMI_TARGET_ARCHITECTURE GetSpmiTargetArchitecture()
{
    return SpmiTargetArchitecture;
}

void SetSpmiTargetArchitecture(SPMI_TARGET_ARCHITECTURE spmiTargetArchitecture)
{
    SpmiTargetArchitecture = spmiTargetArchitecture;
}

// The following functions are used for arm64/arm32 relocation processing.
// They are copies of the code in src\coreclr\utilcode\util.cpp.
// We decided to copy them instead of linking with utilcode library
// to avoid introducing additional runtime dependencies.

void PutArm64Rel28(UINT32* pCode, INT32 imm28)
{
    UINT32 branchInstr = *pCode;
    branchInstr &= 0xFC000000;
    branchInstr |= ((imm28 >> 2) & 0x03FFFFFF);
    *pCode = branchInstr;
}

void PutArm64Rel21(UINT32* pCode, INT32 imm21)
{
    UINT32 adrpInstr = *pCode;
    adrpInstr &= 0x9F00001F;
    INT32 immlo = imm21 & 0x03;
    INT32 immhi = (imm21 & 0x1FFFFC) >> 2;
    adrpInstr |= ((immlo << 29) | (immhi << 5));
    *pCode = adrpInstr;
}

void PutArm64Rel12(UINT32* pCode, INT32 imm12)
{
    UINT32 addInstr = *pCode;
    addInstr &= 0xFFC003FF;
    addInstr |= (imm12 << 10);
    *pCode = addInstr;
}

void PutThumb2Imm16(UINT16* p, UINT16 imm16)
{
    USHORT Opcode0 = p[0];
    USHORT Opcode1 = p[1];
    Opcode0 &= ~((0xf000 >> 12) | (0x0800 >> 1));
    Opcode1 &= ~((0x0700 << 4) | (0x00ff << 0));
    Opcode0 |= (imm16 & 0xf000) >> 12;
    Opcode0 |= (imm16 & 0x0800) >> 1;
    Opcode1 |= (imm16 & 0x0700) << 4;
    Opcode1 |= (imm16 & 0x00ff) << 0;
    p[0] = Opcode0;
    p[1] = Opcode1;
}

void PutThumb2Mov32(UINT16* p, UINT32 imm32)
{
    PutThumb2Imm16(p, (UINT16)imm32);
    PutThumb2Imm16(p + 2, (UINT16)(imm32 >> 16));
}

void PutThumb2BlRel24(UINT16* p, INT32 imm24)
{
    USHORT Opcode0 = p[0];
    USHORT Opcode1 = p[1];
    Opcode0 &= 0xF800;
    Opcode1 &= 0xD000;

    UINT32 S = (imm24 & 0x1000000) >> 24;
    UINT32 J1 = ((imm24 & 0x0800000) >> 23) ^ S ^ 1;
    UINT32 J2 = ((imm24 & 0x0400000) >> 22) ^ S ^ 1;

    Opcode0 |= ((imm24 & 0x03FF000) >> 12) | (S << 10);
    Opcode1 |= ((imm24 & 0x0000FFE) >> 1) | (J1 << 13) | (J2 << 11);

    p[0] = Opcode0;
    p[1] = Opcode1;
}

// GetArm64MovConstant / GetArm64MovkConstant: Decode arm64 mov / movk instructions, e.g.:
//    d29ff600 mov     x0, #65456
//    f2ab8640 movk    x0, #23602, lsl #16
//    f2c04bc0 movk    x0, #606, lsl #32
//
// This is used in the NearDiffer to determine if a sequence of mov/movk is actually an address.
//
// Return `true` if the instruction pointed to by `p` is a mov/movk, `false` otherwise.
// If true, fill out the target register in `*pReg`, constant in `*pCon`, and (for movk) shift value in `*pShift`.

bool GetArm64MovConstant(UINT32* p, unsigned* pReg, unsigned* pCon)
{
    UINT32 instr = *p;
    if ((instr & 0xffe00000) == 0xd2800000)
    {
        *pReg = instr & 0x1f;
        *pCon = (instr >> 5) & 0xffff;
        return true;
    }

    return false;
}

bool GetArm64MovkConstant(UINT32* p, unsigned* pReg, unsigned* pCon, unsigned* pShift)
{
    UINT32 instr = *p;
    if ((instr & 0xff800000) == 0xf2800000)
    {
        *pReg = instr & 0x1f;
        *pCon = (instr >> 5) & 0xffff;
        *pShift = ((instr >> 21) & 0x3) * 16;
        return true;
    }

    return false;
}

// PutArm64MovkConstant: set the constant field in an Arm64 `movk` instruction
void PutArm64MovkConstant(UINT32* p, unsigned con)
{
    *p = (*p & ~(0xffff << 5)) | ((con & 0xffff) << 5);
}

// GetArm32MovwConstant / GetArm32MovtConstant: Decode Arm32 movw / movt instructions, e.g.:
//    4b f2 33 40    movw    r0, #46131
//    c0 f2 79 30    movt    r0, #889
//
// Return `true` if the instruction pointed to by `p` is a movw/movt, `false` otherwise.
// If true, fill out the target register in `*pReg`, constant in `*pCon`.

bool GetArm32MovwConstant(UINT32* p, unsigned* pReg, unsigned* pCon)
{
    // This decodes the "MOV (immediate)" instruction, Encoding T3, from the ARM manual section A8.8.102.
    if (!Is32BitThumb2Instruction((UINT16*)p))
    {
        return false;
    }

    // A Thumb-2 instruction is one or two 16-bit words (in little-endian format).
    UINT16 instr1 = *(UINT16*)p;
    UINT16 instr2 = *((UINT16*)p + 1);
    UINT32 instr = (instr1 << 16) | instr2;
    if ((instr & 0xfbf08000) != 0xf2400000)
    {
        return false;
    }

    *pReg = (instr & 0xf00) >> 8;
    *pCon = ExtractArm32MovImm(instr);
    return true;
}

bool GetArm32MovtConstant(UINT32* p, unsigned* pReg, unsigned* pCon)
{
    // This decodes the "MOVT" instruction, Encoding T1, from the ARM manual section A8.8.106.
    if (!Is32BitThumb2Instruction((UINT16*)p))
    {
        return false;
    }

    // A Thumb-2 instruction is one or two 16-bit words (in little-endian format).
    UINT16 instr1 = *(UINT16*)p;
    UINT16 instr2 = *((UINT16*)p + 1);
    UINT32 instr = (instr1 << 16) | instr2;
    if ((instr & 0xfbf08000) != 0xf2C00000)
    {
        return false;
    }

    *pReg = (instr & 0xf00) >> 8;
    *pCon = ExtractArm32MovImm(instr);
    return true;
}

// Is the instruction we're pointing to an Arm32 (Thumb-2) 32-bit instruction?
bool Is32BitThumb2Instruction(UINT16* p)
{
    UINT16 instr1 = *p;
    if ((instr1 & 0xf800) < 0xe800)
    {
        // Not a 32-bit instruction
        return false;
    }
    return true;
}

// Extract the immediate value from a movw/movt instruction encoding.
UINT32 ExtractArm32MovImm(UINT32 instr)
{
    UINT32 imm4 = (instr >> 16) & 0xf;
    UINT32 i    = (instr >> 26) & 0x1;
    UINT32 imm3 = (instr >> 12) & 0x7;
    UINT32 imm8 = instr & 0xff;
    return (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
}

// PutArm32MovtConstant: set the constant field in an Arm32 `movt` instruction.
// `*p` points to a `movt` instruction. `con` must be a 16-bit constant.
void PutArm32MovtConstant(UINT32* p, unsigned con)
{
    UINT32 imm4 = (con >> 12) & 0xf;
    UINT32 i    = (con >> 11) & 0x1;
    UINT32 imm3 = (con >> 8) & 0x7;
    UINT32 imm8 = con & 0xff;
    UINT16 instr1 = *(UINT16*)p;
    UINT16 instr2 = *((UINT16*)p + 1);
    UINT32 instr = (instr1 << 16) | instr2;
    instr = (instr & 0xfbf08f00) | (imm4 << 16) | (i << 26) | (imm3 << 12) | imm8;
    *(UINT16*)p       = (UINT16)(instr >> 16);
    *((UINT16*)p + 1) = (UINT16)instr;
}

template<typename TPrint>
static std::string getFromPrinter(TPrint print)
{
    char buffer[256];

    size_t requiredBufferSize;
    print(buffer, sizeof(buffer), &requiredBufferSize);

    if (requiredBufferSize <= sizeof(buffer))
    {
        return std::string(buffer);
    }
    else
    {
        std::vector<char> vec(requiredBufferSize);
        size_t printed = print(vec.data(), requiredBufferSize, nullptr);
        assert(printed == requiredBufferSize - 1);
        return std::string(vec.data());
    }
}

std::string getMethodName(MethodContext* mc, CORINFO_METHOD_HANDLE methHnd)
{
    return getFromPrinter([&](char* buffer, size_t bufferSize, size_t* requiredBufferSize) {
        return mc->repPrintMethodName(methHnd, buffer, bufferSize, requiredBufferSize);
        });
}

std::string getClassName(MethodContext* mc, CORINFO_CLASS_HANDLE clsHnd)
{
    return getFromPrinter([&](char* buffer, size_t bufferSize, size_t* requiredBufferSize) {
        return mc->repPrintClassName(clsHnd, buffer, bufferSize, requiredBufferSize);
        });
}

std::string ConvertToUtf8(const WCHAR* str)
{
    unsigned len = WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);
    if (len == 0)
        return{};

    std::vector<char> buf(len + 1);
    WideCharToMultiByte(CP_UTF8, 0, str, -1, buf.data(), len + 1, nullptr, nullptr);
    return std::string{ buf.data() };
}
