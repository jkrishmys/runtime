// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//

#ifndef __ARGDESTINATION_H__
#define __ARGDESTINATION_H__

// The ArgDestination class represents a destination location of an argument.
class ArgDestination
{
    // Base address to which the m_offset is applied to get the actual argument location.
    PTR_VOID m_base;
    // Offset of the argument relative to the m_base. On AMD64 on Unix, it can have a special
    // value that represent a struct that contain both general purpose and floating point fields
    // passed in registers.
    int m_offset;
    // For structs passed in registers, this member points to an ArgLocDesc that contains
    // details on the layout of the struct in general purpose and floating point registers.
    ArgLocDesc* m_argLocDescForStructInRegs;

public:

    // Construct the ArgDestination
    ArgDestination(PTR_VOID base, int offset, ArgLocDesc* argLocDescForStructInRegs)
    :   m_base(base),
        m_offset(offset),
        m_argLocDescForStructInRegs(argLocDescForStructInRegs)
    {
        LIMITED_METHOD_CONTRACT;
#if defined(UNIX_AMD64_ABI)
        _ASSERTE((argLocDescForStructInRegs != NULL) || (offset != TransitionBlock::StructInRegsOffset));
#elif defined(TARGET_ARM64) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
        // This assert is not interesting on arm64/loongarch64. argLocDescForStructInRegs could be
        // initialized if the args are being enregistered.
#else
        _ASSERTE(argLocDescForStructInRegs == NULL);
#endif
    }

    // Get argument destination address for arguments that are not structs passed in registers.
    PTR_VOID GetDestinationAddress()
    {
        LIMITED_METHOD_CONTRACT;
        return dac_cast<PTR_VOID>(dac_cast<TADDR>(m_base) + m_offset);
    }

    bool IsIsFloatArgumentRegister()
    {
        LIMITED_METHOD_CONTRACT;
	return TransitionBlock::IsFloatArgumentRegisterOffset(m_offset);
    }

#if defined(TARGET_ARM64)
#ifndef DACCESS_COMPILE

    // Returns true if the ArgDestination represents an HFA struct
    bool IsHFA()
    {
        return m_argLocDescForStructInRegs != NULL;
    }

    // Copy struct argument into registers described by the current ArgDestination.
    // Arguments:
    //  src = source data of the structure
    //  fieldBytes - size of the structure
    void CopyHFAStructToRegister(void *src, int fieldBytes)
    {
        // We are copying a float, double or vector HFA/HVA and need to
        // enregister each field.

        int floatRegCount = m_argLocDescForStructInRegs->m_cFloatReg;
        int hfaFieldSize = m_argLocDescForStructInRegs->m_hfaFieldSize;
        UINT64* dest = (UINT64*) this->GetDestinationAddress();

        for (int i = 0; i < floatRegCount; ++i)
        {
            // Copy 4 or 8 bytes from src.
            UINT64 val = (hfaFieldSize == 4) ? *((UINT32*)src) : *((UINT64*)src);
            // Always store 8 bytes
            *(dest++) = val;
            // Either zero the next 8 bytes or get the next 8 bytes from src for 16-byte vector.
            *(dest++) = (hfaFieldSize == 16) ? *((UINT64*)src + 1) : 0;

            // Increment src by the appropriate amount.
            src = (void*)((char*)src + hfaFieldSize);
        }
    }

#endif // !DACCESS_COMPILE
#endif // defined(TARGET_ARM64)

#if defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
    bool IsStructPassedInRegs()
    {
        return m_argLocDescForStructInRegs != NULL;
    }

#ifndef DACCESS_COMPILE
    // Copy struct argument into registers described by the current ArgDestination.
    // Arguments:
    //  src = source data of the structure
    //  fieldBytes - size of the structure
    //  destOffset - nonzero when copying values into Nullable<T>, it is the offset
    //               of the T value inside of the Nullable<T>
    void CopyStructToRegisters(void *src, int fieldBytes, int destOffset)
    {
        static const INT64 NanBox =
        #ifdef TARGET_RISCV64
            0xffffffff00000000L;
        #else
            0L;
        #endif // TARGET_RISCV64

        _ASSERTE(IsStructPassedInRegs());
        _ASSERTE(destOffset == 0);
        _ASSERTE(fieldBytes <= 16);

        using namespace FpStruct;
        FpStructInRegistersInfo info = m_argLocDescForStructInRegs->m_structFields;
        _ASSERTE(m_argLocDescForStructInRegs->m_cFloatReg == ((info.flags & BothFloat) ? 2 : 1));
        _ASSERTE(m_argLocDescForStructInRegs->m_cGenReg == ((info.flags & (FloatInt | IntFloat)) ? 1 : 0));
        _ASSERTE(info.offset2nd + info.Size2nd() <= fieldBytes);

        int floatRegOffset = TransitionBlock::GetOffsetOfFloatArgumentRegisters() +
            m_argLocDescForStructInRegs->m_idxFloatReg * FLOAT_REGISTER_SIZE;
        INT64* floatReg = (INT64*)((char*)m_base + floatRegOffset);
        static_assert(sizeof(*floatReg) == FLOAT_REGISTER_SIZE, "");

        if (info.flags & (OnlyOne | BothFloat | FloatInt)) // copy first floating field
        {
            void* field = (char*)src + info.offset1st;
            *floatReg++ = (info.SizeShift1st() == 3) ? *(INT64*)field : NanBox | *(INT32*)field;
        }

        if (info.flags & (BothFloat | IntFloat)) // copy second floating field
        {
            void* field = (char*)src + info.offset2nd;
            *floatReg = (info.SizeShift2nd() == 3) ? *(INT64*)field : NanBox | *(INT32*)field;
        }

        if (info.flags & (FloatInt | IntFloat)) // copy integer field
        {
            int intRegOffset = TransitionBlock::GetOffsetOfArgumentRegisters() +
                m_argLocDescForStructInRegs->m_idxGenReg * TARGET_POINTER_SIZE;
            void* intReg = (char*)m_base + intRegOffset;

            // Unlike passing primitives on RISC-V, the integer field of a struct passed by hardware floating-point
            // calling convention is not type-extended to full register length. Trash the upper bits so the callee
            // accidentally assuming it is extended consistently gets a bad value.
            RISCV64_ONLY(INDEBUG(*(INT64*)intReg = 0xDadAddedC0ffee00l;))

            uint32_t offset = (info.flags & IntFloat) ? info.offset1st : info.offset2nd;
            void* field = (char*)src + offset;
            unsigned sizeShift = (info.flags & IntFloat) ? info.SizeShift1st() : info.SizeShift2nd();
            switch (sizeShift)
            {
                case 0: *(INT8* )intReg = *(INT8* )field; break;
                case 1: *(INT16*)intReg = *(INT16*)field; break;
                case 2: *(INT32*)intReg = *(INT32*)field; break;
                case 3: *(INT64*)intReg = *(INT64*)field; break;
                default: _ASSERTE(false);
            }
        }
    }

#ifdef TARGET_RISCV64
    void CopySingleFloatToRegister(void* src)
    {
        void* dest = GetDestinationAddress();
        UINT32 value = *(UINT32*)src;
        if (TransitionBlock::IsFloatArgumentRegisterOffset(m_offset))
        {
            // NaN-box the floating register value or single-float instructions will treat it as NaN
            *(UINT64*)dest = 0xffffffff00000000L | value;
        }
        else
        {
            // When a single float is passed according to integer calling convention
            // (in integer register or on stack), the upper bits are not specified.
            *(UINT32*)dest = value;
        }
    }
#endif // TARGET_RISCV64

#endif // !DACCESS_COMPILE

    PTR_VOID GetStructGenRegDestinationAddress()
    {
        _ASSERTE(IsStructPassedInRegs());
        int argOfs = TransitionBlock::GetOffsetOfArgumentRegisters() + m_argLocDescForStructInRegs->m_idxGenReg * 8;
        return dac_cast<PTR_VOID>(dac_cast<TADDR>(m_base) + argOfs);
    }
#endif // defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)

#if defined(UNIX_AMD64_ABI)

    // Returns true if the ArgDestination represents a struct passed in registers.
    bool IsStructPassedInRegs()
    {
        LIMITED_METHOD_CONTRACT;
        return m_offset == TransitionBlock::StructInRegsOffset;
    }

    // Get destination address for floating point fields of a struct passed in registers.
    PTR_VOID GetStructFloatRegDestinationAddress()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(IsStructPassedInRegs());
        int offset = TransitionBlock::GetOffsetOfFloatArgumentRegisters() + m_argLocDescForStructInRegs->m_idxFloatReg * 16;
        return dac_cast<PTR_VOID>(dac_cast<TADDR>(m_base) + offset);
    }

    // Get destination address for non-floating point fields of a struct passed in registers.
    PTR_VOID GetStructGenRegDestinationAddress()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(IsStructPassedInRegs());
        int offset = TransitionBlock::GetOffsetOfArgumentRegisters() + m_argLocDescForStructInRegs->m_idxGenReg * 8;
        return dac_cast<PTR_VOID>(dac_cast<TADDR>(m_base) + offset);
    }

#ifndef DACCESS_COMPILE
    // Zero struct argument stored in registers described by the current ArgDestination.
    // Arguments:
    //  fieldBytes - size of the structure
    void ZeroStructInRegisters(int fieldBytes)
    {
        STATIC_CONTRACT_NOTHROW;
        STATIC_CONTRACT_GC_NOTRIGGER;
        STATIC_CONTRACT_FORBID_FAULT;
        STATIC_CONTRACT_MODE_COOPERATIVE;

        // To zero the struct, we create a zero filled array of large enough size and
        // then copy it to the registers. It is implemented this way to keep the complexity
        // of dealing with the eightbyte classification in single function.
        // This function is used rarely and so the overhead of reading the zeros from
        // the stack is negligible.
        long long zeros[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS] = {};
        _ASSERTE(sizeof(zeros) >= (size_t)fieldBytes);

        CopyStructToRegisters(zeros, fieldBytes, 0);
    }

    // Copy struct argument into registers described by the current ArgDestination.
    // Arguments:
    //  src = source data of the structure
    //  fieldBytes - size of the structure
    //  destOffset - nonzero when copying values into Nullable<T>, it is the offset
    //               of the T value inside of the Nullable<T>
    void CopyStructToRegisters(void *src, int fieldBytes, int destOffset)
    {
        STATIC_CONTRACT_NOTHROW;
        STATIC_CONTRACT_GC_NOTRIGGER;
        STATIC_CONTRACT_FORBID_FAULT;
        STATIC_CONTRACT_MODE_COOPERATIVE;

        _ASSERTE(IsStructPassedInRegs());

        BYTE* genRegDest = (BYTE*)GetStructGenRegDestinationAddress() + destOffset;
        BYTE* floatRegDest = (BYTE*)GetStructFloatRegDestinationAddress();
        INDEBUG(int remainingBytes = fieldBytes;)

        EEClass* eeClass = m_argLocDescForStructInRegs->m_eeClass;
        _ASSERTE(eeClass != NULL);

        // We start at the first eightByte that the destOffset didn't skip completely.
        for (int i = destOffset / 8; i < eeClass->GetNumberEightBytes(); i++)
        {
            int eightByteSize = eeClass->GetEightByteSize(i);
            SystemVClassificationType eightByteClassification = eeClass->GetEightByteClassification(i);

            // Adjust the size of the first eightByte by the destOffset
            eightByteSize -= (destOffset & 7);
            destOffset = 0;

            _ASSERTE(remainingBytes >= eightByteSize);

            if (eightByteClassification == SystemVClassificationTypeSSE)
            {
                if (eightByteSize == 8)
                {
                    *(UINT64*)floatRegDest = *(UINT64*)src;
                }
                else
                {
                    _ASSERTE(eightByteSize == 4);
                    *(UINT32*)floatRegDest = *(UINT32*)src;
                }
                floatRegDest += 16;
            }
            else
            {
                if (eightByteSize == 8)
                {
                    _ASSERTE((eightByteClassification == SystemVClassificationTypeInteger) ||
                             (eightByteClassification == SystemVClassificationTypeIntegerReference) ||
                             (eightByteClassification == SystemVClassificationTypeIntegerByRef));

                    _ASSERTE(IS_ALIGNED((SIZE_T)genRegDest, 8));
                    *(UINT64*)genRegDest = *(UINT64*)src;
                }
                else
                {
                    _ASSERTE(eightByteClassification == SystemVClassificationTypeInteger);
                    memcpyNoGCRefs(genRegDest, src, eightByteSize);
                }

                genRegDest += eightByteSize;
            }

            src = (BYTE*)src + eightByteSize;
            INDEBUG(remainingBytes -= eightByteSize;)
        }

        _ASSERTE(remainingBytes == 0);
    }

#endif //DACCESS_COMPILE

    // Report managed object pointers in the struct in registers
    // Arguments:
    //  fn - promotion function to apply to each managed object pointer
    //  sc - scan context to pass to the promotion function
    //  fieldBytes - size of the structure
    void ReportPointersFromStructInRegisters(promote_func *fn, ScanContext *sc, int fieldBytes)
    {
        LIMITED_METHOD_CONTRACT;

       _ASSERTE(IsStructPassedInRegs());

        TADDR genRegDest = dac_cast<TADDR>(GetStructGenRegDestinationAddress());
        INDEBUG(int remainingBytes = fieldBytes;)

        EEClass* eeClass = m_argLocDescForStructInRegs->m_eeClass;
        _ASSERTE(eeClass != NULL);

        for (int i = 0; i < eeClass->GetNumberEightBytes(); i++)
        {
            int eightByteSize = eeClass->GetEightByteSize(i);
            SystemVClassificationType eightByteClassification = eeClass->GetEightByteClassification(i);

            _ASSERTE(remainingBytes >= eightByteSize);

            if (eightByteClassification != SystemVClassificationTypeSSE)
            {
                if ((eightByteClassification == SystemVClassificationTypeIntegerReference) ||
                    (eightByteClassification == SystemVClassificationTypeIntegerByRef))
                {
                    _ASSERTE(eightByteSize == 8);
                    _ASSERTE(IS_ALIGNED((SIZE_T)genRegDest, 8));

                    uint32_t flags = eightByteClassification == SystemVClassificationTypeIntegerByRef ? GC_CALL_INTERIOR : 0;
                    (*fn)(dac_cast<PTR_PTR_Object>(genRegDest), sc, flags);
                }

                genRegDest += eightByteSize;
            }

            INDEBUG(remainingBytes -= eightByteSize;)
        }

        _ASSERTE(remainingBytes == 0);
    }

#endif // UNIX_AMD64_ABI

};

#endif // __ARGDESTINATION_H__
