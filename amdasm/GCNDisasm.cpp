/*
 *  CLRadeonExtender - Unofficial OpenCL Radeon Extensions Library
 *  Copyright (C) 2014 Mateusz Szpakowski
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <CLRX/Config.h>
#include <algorithm>
#include <mutex>
#include <CLRX/Utilities.h>
#include <CLRX/Assembler.h>
#include <CLRX/MemAccess.h>
#include "AsmInternals.h"

using namespace CLRX;

static std::once_flag clrxGCNDisasmOnceFlag; 
static GCNInstruction* gcnInstrTableByCode = nullptr;

struct GCNEncodingSpace
{
    cxuint offset;
    cxuint instrsNum;
};

static const GCNEncodingSpace gcnInstrTableByCodeSpaces[GCNENC_MAXVAL+1] =
{
    { 0, 0 },
    { 0, 0x80 }, // GCNENC_SOPC, opcode = (7bit)<<16 */
    { 0x0080, 0x80 }, // GCNENC_SOPP, opcode = (7bit)<<16 */
    { 0x0100, 0x100 }, /* GCNENC_SOP1, opcode = (8bit)<<8 */
    { 0x0200, 0x80 }, /* GCNENC_SOP2, opcode = (7bit)<<23 */
    { 0x0280, 0x20 }, /* GCNENC_SOPK, opcode = (5bit)<<23 */
    { 0x02a0, 0x40 }, /* GCNENC_SMRD, opcode = (6bit)<<22 */
    { 0x02e0, 0x100 }, /* GCNENC_VOPC, opcode = (8bit)<<27 */
    { 0x03e0, 0x100 }, /* GCNENC_VOP1, opcode = (8bit)<<9 */
    { 0x04e0, 0x40 }, /* GCNENC_VOP2, opcode = (6bit)<<25 */
    { 0x0520, 0x200 }, /* GCNENC_VOP3A, opcode = (9bit)<<17 */
    { 0x0520, 0x200 }, /* GCNENC_VOP3B, opcode = (9bit)<<17 */
    { 0x0720, 0x4 }, /* GCNENC_VINTRP, opcode = (2bit)<<16 */
    { 0x0724, 0x100 }, /* GCNENC_DS, opcode = (8bit)<<18 */
    { 0x0824, 0x80 }, /* GCNENC_MUBUF, opcode = (7bit)<<18 */
    { 0x08a4, 0x8 }, /* GCNENC_MTBUF, opcode = (3bit)<<16 */
    { 0x08ac, 0x80 }, /* GCNENC_MIMG, opcode = (7bit)<<18 */
    { 0x092c, 0x1 }, /* GCNENC_EXP, opcode = none */
    { 0x092d, 0x100 } /* GCNENC_FLAT, opcode = (8bit)<<18 (???8bit) */
};

static const size_t gcnInstrTableByCodeLength = 0x0a2d;

static void initializeGCNDisassembler()
{
    gcnInstrTableByCode = new GCNInstruction[gcnInstrTableByCodeLength];
    for (cxuint i = 0; i < gcnInstrTableByCodeLength; i++)
        gcnInstrTableByCode[i].mnemonic = nullptr;
    
    for (cxuint i = 0; gcnInstrsTable[i].mnemonic != nullptr; i++)
    {
        const GCNInstruction& instr = gcnInstrsTable[i];
        const GCNEncodingSpace& encSpace = gcnInstrTableByCodeSpaces[instr.encoding];
        gcnInstrTableByCode[encSpace.offset] = instr;
    }
}

GCNDisassembler::GCNDisassembler(Disassembler& disassembler)
        : ISADisassembler(disassembler)
{
    std::call_once(clrxGCNDisasmOnceFlag, initializeGCNDisassembler);
}

GCNDisassembler::~GCNDisassembler()
{ }

void GCNDisassembler::beforeDisassemble()
{
    if ((inputSize&3) != 0)
        throw Exception("Input code size must be aligned to 4 bytes!");
    const uint32_t* codeWords = reinterpret_cast<const uint32_t*>(input);
    
    for (size_t pos = 0; pos < (inputSize>>2);)
    {   /* scan all instructions and get jump addresses */
        const uint32_t insnCode = codeWords[pos];
        if ((insnCode & 0x80000000U) != 0)
        {   
            if ((insnCode & 0x40000000U) == 0)
            {   // SOP???
                if  ((insnCode & 0x30000000U) == 0x30000000U)
                {   // SOP1/SOPK/SOPC/SOPP
                    const uint32_t encPart = (insnCode & 0x0f800000U);
                    if (encPart == 0x0e800000U)
                    {   // SOP1
                        if ((insnCode&0xff) == 0xff) // literal
                            pos++;
                    }
                    else if (encPart == 0x0f000000U)
                    {   // SOPC
                        if ((insnCode&0xff) == 0xff ||
                            (insnCode&0xff00) == 0xff00) // literal
                            pos++;
                    }
                    else if (encPart == 0x0f800000U)
                    {   // SOPP
                        const cxuint opcode = (insnCode>>16)&0x7f;
                        if (opcode == 2 || (opcode >= 2 && opcode <= 9))
                            // if jump
                            labels.push_back((pos+int16_t(insnCode&0xffff)+4)<<2);
                    }
                    else
                    {   // SOPK
                        if (((insnCode>>23)&0x1f) == 17)
                            // if branch fork
                            labels.push_back((pos+int16_t(insnCode&0xffff)+1)<<2);
                    }
                }
                else
                {   // SOP2
                    if ((insnCode&0xff) == 0xff) // literal
                        pos++;
                }
            }
            else
            {   // SMRD and others
                const uint32_t encPart = (insnCode&0x3c000000U);
                if (encPart == 0x10000000U) // VOP3
                    pos++;
                else if (encPart == 0x18000000U || encPart == 0x1c000000U ||
                        encPart == 0x20000000U || encPart == 0x28000000U ||
                        encPart == 0x30000000U || encPart == 0x38000000U)
                    pos++; // all DS,FLAT,MUBUF,MTBUF,MIMG,EXP have 8-byte opcode
            }
        }
        else
        {   // some vector instructions
            if ((insnCode & 0x7e000000U) == 0x7c000000U)
            {   // VOPC
                if ((insnCode&0x1ff) == 0xff || (insnCode&0x3fe00) == 0x1fe00) // literal
                    pos++;
            }
            else if ((insnCode & 0x7e000000U) == 0x7e000000U)
            {   // VOP1
                if ((insnCode&0x1ff) == 0xff) // literal
                    pos++;
            }
            else
            {   // VOP2
                const cxuint opcode = (insnCode >> 25)&0x3f;
                if (opcode == 32 || opcode == 33) // V_MADMK and V_MADAK
                    pos++;  // inline 32-bit constant
                else if ((insnCode&0x1ff) == 0xff || (insnCode&0x3fe00) == 0x1fe00)
                    pos++;  // literal
            }
        }
        pos++;
    }
    std::sort(labels.begin(), labels.end());
    const auto newEnd = std::unique(labels.begin(), labels.end());
    labels.resize(newEnd-labels.begin());
}

static const cxbyte gcnEncoding11Table[16] =
{
    GCNENC_SMRD, // 0000
    GCNENC_SMRD, // 0001
    GCNENC_VINTRP, // 0010
    GCNENC_SMRD, // 0011
    GCNENC_VOP3A, // 0100
    GCNENC_SMRD, // 0101
    GCNENC_DS,   // 0110
    GCNENC_FLAT, // 0111
    GCNENC_MUBUF, // 1000
    GCNENC_SMRD,  // 1001
    GCNENC_MTBUF, // 1010
    GCNENC_SMRD,  // 1011
    GCNENC_MIMG,  // 1100
    GCNENC_SMRD,  // 1101
    GCNENC_EXP,   // 1110
    GCNENC_SMRD   // 1111
};

struct GCNEncodingOpcodeBits
{
    cxbyte bitPos;
    cxbyte bits;
};

static const GCNEncodingOpcodeBits gcnEncodingOpcodeTable[GCNENC_MAXVAL+1] =
{
    { 0, 0 },
    { 16, 7 }, /* GCNENC_SOPC, opcode = (7bit)<<16 */
    { 16, 7 }, /* GCNENC_SOPP, opcode = (7bit)<<16 */
    { 8, 8 }, /* GCNENC_SOP1, opcode = (8bit)<<8 */
    { 23, 7 }, /* GCNENC_SOP2, opcode = (7bit)<<23 */
    { 23, 5 }, /* GCNENC_SOPK, opcode = (5bit)<<23 */
    { 22, 6 }, /* GCNENC_SMRD, opcode = (6bit)<<22 */
    { 17, 8 }, /* GCNENC_VOPC, opcode = (8bit)<<17 */
    { 9, 8 }, /* GCNENC_VOP1, opcode = (8bit)<<9 */
    { 25, 6 }, /* GCNENC_VOP2, opcode = (6bit)<<25 */
    { 17, 9 }, /* GCNENC_VOP3A, opcode = (9bit)<<17 */
    { 17, 9 }, /* GCNENC_VOP3B, opcode = (9bit)<<17 */
    { 16, 2 }, /* GCNENC_VINTRP, opcode = (2bit)<<16 */
    { 18, 8 }, /* GCNENC_DS, opcode = (8bit)<<18 */
    { 18, 7 }, /* GCNENC_MUBUF, opcode = (7bit)<<18 */
    { 16, 3 }, /* GCNENC_MTBUF, opcode = (3bit)<<16 */
    { 18, 7 }, /* GCNENC_MIMG, opcode = (7bit)<<18 */
    { 0, 0 }, /* GCNENC_EXP, opcode = none */
    { 18, 8 } /* GCNENC_FLAT, opcode = (8bit)<<18 (???8bit) */
};

static const char* gcnOperandFloatTable[] =
{
    "0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"
};

union FloatUnion
{
    float f;
    uint32_t u;
};

static size_t decodeGCNOperand(cxuint op, cxuint vregNum, char* buf, cxuint literal = 0,
               bool floatLit = false)
{
    if (op < 104 || (op >= 256 && op < 512))
    {   // scalar
        if (op >= 256)
        {
            buf[0] = 'v';
            op -= 256;
        }
        else
            buf[0] = 's';
        size_t pos = 1;
        if (vregNum!=1)
            buf[pos++] = '[';
        cxuint val = op;
        if (val >= 100)
        {
            cxuint digit2 = val/100;
            buf[pos++] = digit2+'0';
            val -= digit2*100;
        }
        {
            const cxuint digit1 = val/10;
            if (digit1 != 0)
                buf[pos++] = digit1 + '0';
            buf[pos++] = val-digit1*10 + '0';
        }
        if (vregNum!=1)
        {
            buf[pos++] = ':';
            op += vregNum-1;
            val = op;
            if (val >= 100)
            {
                cxuint digit2 = val/100;
                buf[pos++] = digit2+'0';
                val -= digit2*100;
            }
            {
                const cxuint digit1 = val/10;
                if (digit1 != 0)
                    buf[pos++] = digit1 + '0';
                buf[pos++] = val-digit1*10 + '0';
            }
            buf[pos++] = ']';
        }
        return pos;
    }
    
    const cxuint op2 = op&~1U;
    if (op2 == 106 || op2 == 108 || op2 == 110 || op2 == 126)
    {   // VCC
        size_t pos = 0;
        switch(op2)
        {
            case 106:
                buf[pos++] = 'v';
                buf[pos++] = 'c';
                buf[pos++] = 'c';
                break;
            case 108:
                buf[pos++] = 't';
                buf[pos++] = 'b';
                buf[pos++] = 'a';
                break;
            case 110:
                buf[pos++] = 't';
                buf[pos++] = 'm';
                buf[pos++] = 'a';
                break;
            case 126:
                buf[pos++] = 'e';
                buf[pos++] = 'x';
                buf[pos++] = 'e';
                buf[pos++] = 'c';
                break;
        }
        if (vregNum == 2)
        {
            if (op == 107) // unaligned!!
            {
                buf[pos++] = '_';
                buf[pos++] = 'u';
                buf[pos++] = '!';
                return pos;
            }
            return 3;
        }
        buf[pos++] = '_';
        if ((op&1) == 0)
        { buf[pos++] = 'l'; buf[pos++] = 'o'; }
        else
        { buf[pos++] = 'h'; buf[pos++] = 'i'; }
        return pos;
    }
    
    if (op == 255) // if literal
    {
        size_t pos = u32tocstrCStyle(literal, buf, 11, 16);
        if (floatLit)
        {
            FloatUnion fu;
            fu.u = literal;
            buf[pos++] = ' ';
            buf[pos++] = '/';
            buf[pos++] = '*';
            buf[pos++] = ' ';
            pos += ftocstrCStyle(fu.f, buf+pos, 27);
            buf[pos++] = ' ';
            buf[pos++] = '*';
            buf[pos++] = '/';
        }
        return pos;
    }
    
    if (op >= 112 && op < 124)
    {
        buf[0] = 't';
        buf[1] = 't';
        buf[2] = 'm';
        buf[3] = 'p';
        cxuint pos = 4;
        if (op >= 122)
        {
            buf[pos++] = '1';
            buf[pos++] = op-122+'0';
        }
        else
            buf[pos++] = op-112+'0';
        return pos;
    }
    if (op == 124)
    {
        buf[0] = 'm';
        buf[1] = '0';
        return 2;
    }
    
    if (op >= 128 && op <= 192)
    {   // integer constant
        size_t pos = 0;
        op -= 128;
        const cxuint digit1 = op/10;
        if (digit1 != 0)
            buf[pos++] = digit1 + '0';
        buf[pos++] = op-digit1*10 + '0';
        return pos;
    }
    if (op > 192 && op <= 208)
    {
        buf[0] = '-';
        cxuint pos = 1;
        if (op >= 122)
        {
            buf[pos++] = '1';
            buf[pos++] = op-202+'0';
        }
        else
            buf[pos++] = op-192+'0';
        return pos;
    }
    if (op >= 240 && op < 248)
    {
        size_t pos = 0;
        const char* inOp = gcnOperandFloatTable[op-240];
        for (pos = 0; inOp[pos] != 0; pos++)
            buf[pos] = inOp[pos];
        return pos;
    }
    
    switch(op)
    {
        case 251:
            buf[0] = 'v';
            buf[1] = 'c';
            buf[2] = 'c';
            buf[3] = 'z';
            return 4;
        case 252:
            buf[0] = 'e';
            buf[1] = 'x';
            buf[2] = 'e';
            buf[3] = 'c';
            buf[4] = 'z';
            return 5;
        case 253:
            buf[0] = 's';
            buf[1] = 'c';
            buf[2] = 'c';
            return 3;
        case 254:
            buf[0] = 'l';
            buf[1] = 'd';
            buf[2] = 's';
            return 3;
    }
    
    // reserved value (illegal)
    buf[0] = 'i';
    buf[1] = 'l';
    buf[2] = 'l';
    buf[3] = '_';
    buf[4] = '0'+op/100;
    buf[5] = '0'+(op/10)%10;
    buf[6] = '0'+op%10;
    return 7;
}

static const char* sendMsgCodeMessageTable[16] =
{
    "0",
    "interrupt",
    "gs",
    "gs_done",
    "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "system"
};

static const char* sendGsOpMessageTable[4] =
{ "nop", "cut", "emit", "emit-cut" };

void GCNDisassembler::disassemble()
{
    auto curLabel = labels.begin();
    if ((inputSize&3) != 0)
        throw Exception("Input code size must be aligned to 4 bytes!");
    const uint32_t* codeWords = reinterpret_cast<const uint32_t*>(input);
    
    std::ostream& output = disassembler.getOutput();
    
    char buf[320];
    size_t bufPos = 0;
    const size_t codeWordsNum = (inputSize>>2);
    for (size_t pos = 0; pos < codeWordsNum;)
    {   // check label
        if (curLabel != labels.end() && (pos<<2) == *curLabel)
        {   // put label
            buf[bufPos++] = 'L';
            bufPos += u32tocstrCStyle((pos<<2), buf+bufPos, 11, 10, 0, false);
            buf[bufPos++] = ':';
            buf[bufPos++] = '\n';
            curLabel++;
        }
        
        // add spaces
        for (size_t p = 0; p < 8; p++)
            buf[bufPos+p] = ' ';
        bufPos += 8;
        
        cxbyte gcnEncoding = GCNENC_NONE;
        const uint32_t insnCode = codeWords[pos++];
        uint32_t insn2Code = 0;
        uint32_t literal = 0;
        
        /* determine GCN encoding */
        if ((insnCode & 0x80000000U) != 0)
        {
            if ((insnCode & 0x40000000U) == 0)
            {   // SOP???
                if  ((insnCode & 0x30000000U) == 0x30000000U)
                {   // SOP1/SOPK/SOPC/SOPP
                    const uint32_t encPart = (insnCode & 0x0f800000U);
                    if (encPart == 0x0e800000U)
                    {   // SOP1
                        if ((insnCode&0xff) == 0xff) // literal
                        {
                            if (pos >= codeWordsNum)
                                throw Exception("Instruction outside code space!");
                            literal = codeWords[pos++];
                        }
                        gcnEncoding = GCNENC_SOP1;
                    }
                    else if (encPart == 0x0f000000U)
                    {   // SOPC
                        if ((insnCode&0xff) == 0xff ||
                            (insnCode&0xff00) == 0xff00) // literal
                        {
                            if (pos >= codeWordsNum)
                                throw Exception("Instruction outside code space!");
                            literal = codeWords[pos++];
                        }
                        gcnEncoding = GCNENC_SOPC;
                    }
                    else if (encPart == 0x0f800000U) // SOPP
                        gcnEncoding = GCNENC_SOPP;
                    else // SOPK
                        gcnEncoding = GCNENC_SOPK;
                }
                else
                {   // SOP2
                    if ((insnCode&0xff) == 0xff) // literal
                    {
                        if (pos >= codeWordsNum)
                            throw Exception("Instruction outside code space!");
                        literal = codeWords[pos++];
                    }
                    gcnEncoding = GCNENC_SOP2;
                }
            }
            else
            {   // SMRD and others
                const uint32_t encPart = (insnCode&0x3c000000U);
                if (encPart == 0x10000000U)// VOP3
                {
                    if (pos >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    insn2Code = codeWords[pos++];
                }
                else if (encPart == 0x18000000U || encPart == 0x1c000000U ||
                        encPart == 0x20000000U || encPart == 0x28000000U ||
                        encPart == 0x30000000U || encPart == 0x38000000U)
                {   // all DS,FLAT,MUBUF,MTBUF,MIMG,EXP have 8-byte opcode
                    if (pos+1 >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    insn2Code = codeWords[pos++];
                }
                gcnEncoding = gcnEncoding11Table[(encPart>>26)&0xf];
            }
        }
        else
        {   // some vector instructions
            if ((insnCode & 0x7e000000U) == 0x7c000000U)
            {   // VOPC
                if ((insnCode&0x1ff) == 0xff || (insnCode&0x3fe00) == 0x1fe00) // literal
                {
                    if (pos >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    literal = codeWords[pos++];
                }
                gcnEncoding = GCNENC_VOPC;
            }
            else if ((insnCode & 0x7e000000U) == 0x7e000000U)
            {   // VOP1
                if ((insnCode&0x1ff) == 0xff) // literal
                {
                    if (pos >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    literal = codeWords[pos++];
                }
                gcnEncoding = GCNENC_VOP1;
            }
            else
            {   // VOP2
                const cxuint opcode = (insnCode >> 25)&0x3f;
                if (opcode == 32 || opcode == 33) // V_MADMK and V_MADAK
                {
                    if (pos >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    literal = codeWords[pos++];
                }
                else if ((insnCode&0x1ff) == 0xff || (insnCode&0x3fe00) == 0x1fe00)
                {
                    if (pos >= codeWordsNum)
                        throw Exception("Instruction outside code space!");
                    literal = codeWords[pos++];
                }
                gcnEncoding = GCNENC_VOP2;
            }
        }
        
        if (gcnEncoding == GCNENC_NONE)
        {   // 
        }
        
        const cxuint opcode = (insnCode>>gcnEncodingOpcodeTable[gcnEncoding].bitPos) & 
            ((1U<<gcnEncodingOpcodeTable[gcnEncoding].bits)-1U);
        
        /* decode instruction and put to output */
        const GCNEncodingSpace& encSpace = gcnInstrTableByCodeSpaces[gcnEncoding];
        const GCNInstruction& gcnInsn = gcnInstrTableByCode[encSpace.offset + opcode];
        
        for (size_t k = 0; gcnInsn.mnemonic[k]!=0; k++)
            buf[bufPos++] = gcnInsn.mnemonic[k];
        buf[bufPos++] = ' ';
        
        const bool displayFloatLits = ((disassembler.getFlags()&DISASM_FLOATLITS) != 0 &&
                (gcnInsn.mode & GCN_MASK2) == GCN_FLOATLIT);
        
        switch(gcnEncoding)
        {
            case GCNENC_SOPC:
            {
                bufPos += decodeGCNOperand(insnCode&0xff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand((insnCode>>8)&0xff,
                       (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf + bufPos, literal);
                break;
            }
            case GCNENC_SOPP:
            {
                const cxuint imm16 = insnCode&0xffff;
                switch(gcnInsn.mode&GCN_MASK1)
                {
                    case GCN_IMM_REL:
                    {
                        const size_t branchPos = (pos + int16_t(imm16) + 1)<<2;
#ifdef GCN_DISASM_TEST
                        const auto p =
                                std::lower_bound(labels.begin(), labels.end(), branchPos);
                        if (p == labels.end() || *p != branchPos)
                            throw Exception("FATAL: Label not found!!!");
#endif
                        buf[bufPos++] = 'L';
                        bufPos += u32tocstrCStyle(branchPos,
                                  buf+bufPos, 11, 10, 0, false);
                        break;
                    }
                    case GCN_IMM_LOCKS:
                    {
                        bool prevLock = false;
                        if ((imm16&15) != 15)
                        {
                            const cxuint lockCnt = imm16&15;
                            buf[bufPos++] = 'v';
                            buf[bufPos++] = 'm';
                            buf[bufPos++] = 'c';
                            buf[bufPos++] = 'n';
                            buf[bufPos++] = 't';
                            if (lockCnt >= 10)
                                buf[bufPos++] = '1';
                            buf[bufPos++] = '0' + (lockCnt>=10)?lockCnt-10:lockCnt;
                            prevLock = true;
                        }
                        if (((imm16>>4)&7) != 7)
                        {
                            if (prevLock)
                            {
                                buf[bufPos++] = ' ';
                                buf[bufPos++] = '&';
                                buf[bufPos++] = ' ';
                            }
                            buf[bufPos++] = 'e';
                            buf[bufPos++] = 'x';
                            buf[bufPos++] = 'p';
                            buf[bufPos++] = 'c';
                            buf[bufPos++] = 'n';
                            buf[bufPos++] = 't';
                            buf[bufPos++] = '0' + ((imm16>>4)&7);
                            prevLock = true;
                        }
                        if (((imm16>>8)&15) != 15)
                        {   /* LGKMCNT bits is 4 */
                            const cxuint lockCnt = (imm16>>8)&15;
                            if (prevLock)
                            {
                                buf[bufPos++] = ' ';
                                buf[bufPos++] = '&';
                                buf[bufPos++] = ' ';
                            }
                            buf[bufPos++] = 'l';
                            buf[bufPos++] = 'g';
                            buf[bufPos++] = 'k';
                            buf[bufPos++] = 'm';
                            buf[bufPos++] = 'c';
                            buf[bufPos++] = 'n';
                            buf[bufPos++] = 't';
                            if (lockCnt >= 10)
                                buf[bufPos++] = '1';
                            buf[bufPos++] = '0' + (lockCnt>=10)?lockCnt-10:lockCnt;
                            prevLock = true;
                        }
                        if ((imm16&0xf080) != 0)
                        {   /* additional info about imm16 */
                            if (prevLock)
                            {
                                buf[bufPos++] = ' ';
                                buf[bufPos++] = ':';
                            }
                            bufPos += u32tocstrCStyle(imm16, buf+bufPos, 11, 16);
                        }
                        break;
                    }
                    case GCN_IMM_MSGS:
                    {
                        cxuint illMask = 0xfff0;
                        buf[bufPos++] = 'm';
                        buf[bufPos++] = 's';
                        buf[bufPos++] = 'g';
                        buf[bufPos++] = '(';
                        const char* msgName = sendMsgCodeMessageTable[imm16&15];
                        while (*msgName != 0)
                            buf[bufPos++] = *msgName++;
                        buf[bufPos++] = ',';
                        buf[bufPos++] = ' ';
                        if ((imm16&14) == 2) // gs ops
                        {
                            illMask = 0xffc0;
                            const char* gsopName = sendGsOpMessageTable[(imm16>>4)&3];
                            while (*gsopName != 0)
                                buf[bufPos++] = *gsopName++;
                            if ((imm16&0x30) != 0)
                            {
                                illMask = 0xfcc0;
                                buf[bufPos++] = ',';
                                buf[bufPos++] = ' ';
                                buf[bufPos++] = '0' + ((imm16>>8)&3);
                            }
                        }
                        buf[bufPos++] = ')';
                        if ((imm16&illMask) != 0)
                        {
                            buf[bufPos++] = ' ';
                            buf[bufPos++] = ':';
                            bufPos += u32tocstrCStyle(imm16, buf+bufPos, 11, 16);
                        }
                        break;
                    }
                    case GCN_IMM_NONE:
                        if (imm16 != 0)
                            bufPos += u32tocstrCStyle(imm16, buf+bufPos, 11, 16);
                        break;
                    default:
                        bufPos += u32tocstrCStyle(imm16, buf+bufPos, 11, 16);
                        break;
                }
                break;
            }
            case GCNENC_SOP1:
            {
                bufPos += decodeGCNOperand((insnCode>>16)&0x7f,
                       (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(insnCode&0xff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal);
                break;
            }
            case GCNENC_SOP2:
            {
                if ((gcnInsn.mode & GCN_MASK1) != GCN_REG_S1_JMP)
                {
                    bufPos += decodeGCNOperand((insnCode>>16)&0x7f,
                           (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos);
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                }
                bufPos += decodeGCNOperand(insnCode&0xff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand((insnCode>>8)&0xff,
                       (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf + bufPos, literal);
                
                if ((gcnInsn.mode & GCN_MASK1) == GCN_REG_S1_JMP &&
                    ((insnCode>>16)&0x7f) != 0)
                {
                    buf[bufPos++] = ' ';
                    buf[bufPos++] = 's';
                    buf[bufPos++] = 'd';
                    buf[bufPos++] = 's';
                    buf[bufPos++] = 't';
                    buf[bufPos++] = '=';
                    bufPos += u32tocstrCStyle(((insnCode>>16)&0x7f), buf+bufPos, 6, 16);
                }
                break;
            }
            case GCNENC_SOPK:
            {
                bufPos += decodeGCNOperand((insnCode>>16)&0x7f,
                       (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                cxuint imm16 = insnCode&0xffff;
                if ((gcnInsn.mode&0xf0) != GCN_IMM_REL)
                    bufPos += u32tocstrCStyle(imm16, buf+bufPos, 11, 16);
                else
                {
                    const size_t branchPos = (pos + int16_t(imm16) + 1)<<2;
#ifdef GCN_DISASM_TEST
                    const auto p =
                            std::lower_bound(labels.begin(), labels.end(), branchPos);
                    if (p == labels.end() || *p != branchPos)
                        throw Exception("FATAL: Label not found!!!");
#endif
                    buf[bufPos++] = 'L';
                    bufPos += u32tocstrCStyle(branchPos,
                              buf+bufPos, 11, 10, 0, false);
                }
                break;
            }
            case GCNENC_SMRD:
            {
                const cxuint dregsNum = 1<<(gcnInsn.mode & 0xf00);
                bufPos += decodeGCNOperand((insnCode>>15)&0x7f, dregsNum, buf + bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand((insnCode>>8)&0x7e,
                           (gcnInsn.mode&GCN_SBASE4)?4:2, buf + bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                if (insnCode&0x100) // immediate value
                    bufPos += u32tocstrCStyle(insnCode&0xff, buf+bufPos, 11, 16);
                else // S register
                    bufPos += decodeGCNOperand(insnCode&0xff, 1, buf + bufPos);
                break;
            }
            case GCNENC_VOPC:
            {
                buf[bufPos++] = 'v';
                buf[bufPos++] = 'c';
                buf[bufPos++] = 'c';
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(insnCode&0x1ff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal,
                               displayFloatLits);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(((insnCode>>9)&0xff)+256,
                       (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf + bufPos, 0);
                break;
            }
            case GCNENC_VOP1:
            {
                bufPos += decodeGCNOperand(((insnCode>>17)&0xff)+256,
                       (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos, 0);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(insnCode&0x1ff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal,
                               displayFloatLits);
                break;
            }
            case GCNENC_VOP2:
            {
                bufPos += decodeGCNOperand(((insnCode>>17)&0xff)+256,
                       (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(insnCode&0x1ff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal,
                               displayFloatLits);
                if ((gcnInsn.mode & GCN_MASK1) == GCN_ARG1_IMM)
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    bufPos += u32tocstrCStyle(literal, buf+bufPos, 11, 16);
                    if (displayFloatLits)
                    {
                        FloatUnion fu;
                        fu.u = literal;
                        buf[bufPos++] = ' ';
                        buf[bufPos++] = '/';
                        buf[bufPos++] = '*';
                        buf[bufPos++] = ' ';
                        bufPos += ftocstrCStyle(fu.f, buf+bufPos, 27);
                        buf[bufPos++] = ' ';
                        buf[bufPos++] = '*';
                        buf[bufPos++] = '/';
                    }
                }
                else if ((gcnInsn.mode & GCN_MASK1) == GCN_DS2_VCC ||
                    (gcnInsn.mode & GCN_MASK1) == GCN_DST_VCC)
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    buf[bufPos++] = 'v';
                    buf[bufPos++] = 'c';
                    buf[bufPos++] = 'c';
                }
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(((insnCode>>9)&0x1ff) + 256,
                       (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf + bufPos);
                if ((gcnInsn.mode & GCN_MASK1) == GCN_ARG2_IMM)
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    bufPos += u32tocstrCStyle(literal, buf+bufPos, 11, 16);
                    if (displayFloatLits)
                    {
                        FloatUnion fu;
                        fu.u = literal;
                        buf[bufPos++] = ' ';
                        buf[bufPos++] = '/';
                        buf[bufPos++] = '*';
                        buf[bufPos++] = ' ';
                        bufPos += ftocstrCStyle(fu.f, buf+bufPos, 27);
                        buf[bufPos++] = ' ';
                        buf[bufPos++] = '*';
                        buf[bufPos++] = '/';
                    }
                }
                else if ((gcnInsn.mode & GCN_MASK1) == GCN_DS2_VCC ||
                    (gcnInsn.mode & GCN_MASK1) == GCN_SRC2_VCC)
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    buf[bufPos++] = 'v';
                    buf[bufPos++] = 'c';
                    buf[bufPos++] = 'c';
                }
                break;
            }
            case GCNENC_VOP3A:
            {
                if (opcode < 256) /* compares */
                    bufPos += decodeGCNOperand(((insnCode>>17)&0xff), 2, buf + bufPos);
                else /* regular instruction */
                    bufPos += decodeGCNOperand(((insnCode>>17)&0xff)+256,
                       (gcnInsn.mode&GCN_REG_DST_64)?2:1, buf + bufPos);
                
                cxuint absFlags = 0;
                if (gcnInsn.encoding == GCNENC_VOP3A)
                    absFlags = (insnCode>>8)&7;
                else if ((gcnInsn.mode & GCN_MASK1) == GCN_DS2_VCC ||
                    (gcnInsn.mode & GCN_MASK1) == GCN_DST_VCC) /* VOP3b */
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    bufPos += decodeGCNOperand(((insnCode>>8)&0x7f), 2, buf + bufPos);
                }
                
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                if ((insn2Code & (1U<<29)) != 0)
                    buf[bufPos++] = '-';
                if (absFlags & 1)
                {
                    buf[bufPos++] = 'a';
                    buf[bufPos++] = 'b';
                    buf[bufPos++] = 's';
                    buf[bufPos++] = '(';
                }
                bufPos += decodeGCNOperand(insn2Code&0x1ff,
                       (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf + bufPos, literal,
                               displayFloatLits);
                if (absFlags & 1)
                    buf[bufPos++] = ')';
                
                if ((gcnInsn.mode & GCN_MASK1) != GCN_SRC12_NONE)
                {
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                    if ((insn2Code & (1U<<30)) != 0)
                        buf[bufPos++] = '-';
                    if (absFlags & 2)
                    {
                        buf[bufPos++] = 'a';
                        buf[bufPos++] = 'b';
                        buf[bufPos++] = 's';
                        buf[bufPos++] = '(';
                    }
                    bufPos += decodeGCNOperand((insn2Code>>9)&0x1ff,
                       (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf + bufPos, literal,
                               displayFloatLits);
                    if (absFlags & 2)
                        buf[bufPos++] = ')';
                    
                    if ((gcnInsn.mode & GCN_MASK1) != GCN_SRC2_NONE)
                    {
                        buf[bufPos++] = ',';
                        buf[bufPos++] = ' ';
                        
                        if ((gcnInsn.mode & GCN_MASK1) == GCN_DS2_VCC ||
                            (gcnInsn.mode & GCN_MASK1) == GCN_SRC2_VCC)
                        {
                            bufPos += decodeGCNOperand((insn2Code>>18)&0x1ff,
                               2, buf + bufPos);
                        }
                        else
                        {
                            if ((insn2Code & (1U<<31)) != 0)
                                buf[bufPos++] = '-';
                            if (absFlags & 4)
                            {
                                buf[bufPos++] = 'a';
                                buf[bufPos++] = 'b';
                                buf[bufPos++] = 's';
                                buf[bufPos++] = '(';
                            }
                            bufPos += decodeGCNOperand((insn2Code>>18)&0x1ff,
                               (gcnInsn.mode&GCN_REG_SRC2_64)?2:1, buf + bufPos, literal,
                                       displayFloatLits);
                            if (absFlags & 4)
                                buf[bufPos++] = ')';
                        }
                    }
                }
                /* as field */
                if ((gcnInsn.mode & GCN_MASK1) == GCN_SRC12_NONE &&
                    ((insn2Code>>9)&0x1ff) != 0)
                {
                    buf[bufPos++] = ' ';
                    buf[bufPos++] = 's';
                    buf[bufPos++] = 'r';
                    buf[bufPos++] = 'c';
                    buf[bufPos++] = '1';
                    buf[bufPos++] = '=';
                    bufPos += u32tocstrCStyle((insn2Code>>9)&0x1ff, buf+bufPos, 6, 16);
                }
                if (((gcnInsn.mode & GCN_MASK1) == GCN_SRC12_NONE ||
                    (gcnInsn.mode & GCN_MASK1) == GCN_SRC2_NONE) &&
                    ((insn2Code>>9)&0x1ff) != 0)
                {
                    buf[bufPos++] = ' ';
                    buf[bufPos++] = 's';
                    buf[bufPos++] = 'r';
                    buf[bufPos++] = 'c';
                    buf[bufPos++] = '2';
                    buf[bufPos++] = '=';
                    bufPos += u32tocstrCStyle((insn2Code>>18)&0x1ff, buf+bufPos, 6, 16);
                }
                break;
            }
            case GCNENC_VINTRP:
            {
                bufPos += decodeGCNOperand(((insnCode>>18)&0xff)+256, 1, buf+bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                bufPos += decodeGCNOperand(insnCode+256, 1, buf+bufPos);
                buf[bufPos++] = ',';
                buf[bufPos++] = ' ';
                buf[bufPos++] = 'a';
                buf[bufPos++] = 't';
                buf[bufPos++] = 't';
                buf[bufPos++] = 'r';
                const cxuint attr = (insnCode>>10)&63;
                if (attr >= 10)
                {
                    const cxuint digit2 = attr/10;
                    buf[bufPos++] = '0' + digit2;
                    buf[bufPos++] = '0' + attr - 10*digit2;
                }
                else
                    buf[bufPos++] = '0' + attr;
                buf[bufPos++] = '.';
                buf[bufPos++] = "xyzw"[((insnCode>>8)&3)]; // attrchannel
                break;
            }
            case GCNENC_DS:
            {
                if ((gcnInsn.mode & GCN_ADDR_DST) != 0) /* address is dst */
                {
                    bufPos += decodeGCNOperand((insn2Code&0xff)+256, 1, buf+bufPos);
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                }
                else
                {   /* vdst is dst */
                    cxuint regsNum = (gcnInsn.mode&GCN_REG_DST_64)?2:1;
                    if ((gcnInsn.mode&GCN_DSMASK) == GCN_ADDR_SRC96)
                        regsNum = 3;
                    if ((gcnInsn.mode&GCN_DSMASK) == GCN_ADDR_SRC128)
                        regsNum = 4;
                    bufPos += decodeGCNOperand((insn2Code>>24)+256, regsNum,
                           buf+bufPos);
                    buf[bufPos++] = ',';
                    buf[bufPos++] = ' ';
                }
                
                if ((gcnInsn.mode & GCN_DSMASK2) != GCN_ONLYDST)
                {   /* two vdata */
                    bufPos += decodeGCNOperand(((insn2Code>>8)&0xff) + 256,
                        (gcnInsn.mode&GCN_REG_SRC0_64)?2:1, buf+bufPos);
                    if ((gcnInsn.mode & GCN_DSMASK2) == GCN_2SRCS ||
                        (gcnInsn.mode & GCN_DSMASK2) == GCN_VDATA2)
                    {
                        buf[bufPos++] = ',';
                        buf[bufPos++] = ' ';
                        bufPos += decodeGCNOperand(((insn2Code>>16)&0xff) + 256,
                            (gcnInsn.mode&GCN_REG_SRC1_64)?2:1, buf+bufPos);
                    }
                }
                
                const cxuint offset = (insnCode&0xffff);
                if (offset != 0)
                {
                    if ((gcnInsn.mode & GCN_DSMASK2) != GCN_VDATA2) /* single offset */
                    {
                        buf[bufPos++] = ' ';
                        buf[bufPos++] = 'o';
                        buf[bufPos++] = 'f';
                        buf[bufPos++] = 'f';
                        buf[bufPos++] = 's';
                        buf[bufPos++] = 'e';
                        buf[bufPos++] = 't';
                        buf[bufPos++] = ':';
                        bufPos += u32tocstrCStyle(offset, buf+bufPos, 7, 10);
                    }
                    else
                    {
                        if ((offset&0xff) != 0)
                        {
                            buf[bufPos++] = ' ';
                            buf[bufPos++] = 'o';
                            buf[bufPos++] = 'f';
                            buf[bufPos++] = 'f';
                            buf[bufPos++] = 's';
                            buf[bufPos++] = 'e';
                            buf[bufPos++] = 't';
                            buf[bufPos++] = '0';
                            buf[bufPos++] = ':';
                            bufPos += u32tocstrCStyle(offset&0xff, buf+bufPos, 7, 10);
                        }
                        if ((offset&0xff00) != 0)
                        {
                            buf[bufPos++] = ' ';
                            buf[bufPos++] = 'o';
                            buf[bufPos++] = 'f';
                            buf[bufPos++] = 'f';
                            buf[bufPos++] = 's';
                            buf[bufPos++] = 'e';
                            buf[bufPos++] = 't';
                            buf[bufPos++] = '1';
                            buf[bufPos++] = ':';
                            bufPos += u32tocstrCStyle((offset>>8)&0xff,
                                      buf+bufPos, 7, 10);
                        }
                    }
                }
                break;
            }
            case GCNENC_MUBUF:
            {
                break;
            }
            case GCNENC_MTBUF:
                break;
            case GCNENC_MIMG:
                break;
            case GCNENC_EXP:
                break;
            case GCNENC_FLAT:
                break;
            default:
                break;
        }
        buf[bufPos++] = '\n';
        
        if (bufPos+100 >= 320)
        {
            output.write(buf, bufPos);
            bufPos = 0;
        }
    }
    /* rest of the labels */
    for (; curLabel != labels.end(); ++curLabel)
    {   // put .org directory
        buf[bufPos++] = '.';
        buf[bufPos++] = 'o';
        buf[bufPos++] = 'r';
        buf[bufPos++] = 'g';
        buf[bufPos++] = ' ';
        bufPos += u32tocstrCStyle(*curLabel, buf+bufPos, 20, 16, 0, false);
        buf[bufPos++] = '\n';
        // put label
        buf[bufPos++] = 'L';
        bufPos += u32tocstrCStyle(*curLabel, buf+bufPos, 11, 10, 0, false);
        buf[bufPos++] = ':';
        buf[bufPos++] = '\n';
    }
}
