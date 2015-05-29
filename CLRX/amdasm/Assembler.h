/*
 *  CLRadeonExtender - Unofficial OpenCL Radeon Extensions Library
 *  Copyright (C) 2014-2015 Mateusz Szpakowski
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
/*! \file Assembler.h
 * \brief an assembler for Radeon GPU's
 */

#ifndef __CLRX_ASSEMBLER_H__
#define __CLRX_ASSEMBLER_H__

#include <CLRX/Config.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <istream>
#include <ostream>
#include <iostream>
#include <vector>
#include <utility>
#include <stack>
#include <unordered_map>
#include <CLRX/amdbin/AmdBinaries.h>
#include <CLRX/amdbin/GalliumBinaries.h>
#include <CLRX/amdbin/AmdBinGen.h>
#include <CLRX/utils/Utilities.h>

/// main namespace
namespace CLRX
{

enum: cxuint
{
    ASM_WARNINGS = 1,   ///< enable all warnings for assembler
    ASM_64BIT_MODE = 2, ///< assemble to 64-bit addressing mode
    ASM_GNU_AS_COMPAT = 4, ///< compatibility with GNU as (expressions)
    ASM_ALL = 0xff  ///< all flags
};
    
enum class AsmFormat: cxbyte
{
    CATALYST = 0,
    GALLIUM
};

enum: cxuint
{
    ASMSECT_ABS = UINT_MAX
};

enum class AsmSectionType: cxbyte
{
    AMD_GLOBAL_DATA = 0,
    AMD_KERNEL_CODE,
    AMD_KERNEL_DATA,
    AMD_KERNEL_HEADER,
    AMD_KERNEL_METADATA,
    
    GALLIUM_GLOBAL_DATA = 64,
    GALLIUM_COMMENT,
    GALLIUM_DISASSEMBLY,
    GALLIUM_CODE
};

class Assembler;

struct LineCol
{
    uint64_t lineNo;
    size_t colNo;
};

/*
 * assembler source position
 */

struct AsmFile: public FastRefCountable
{
    RefPtr<const AsmFile> parent; ///< parent file (or null if root)
    uint64_t lineNo; // place where file is included (0 if root)
    const std::string file; // file path
    
    explicit AsmFile(const std::string& _file) : lineNo(1), file(_file)
    { }
    
    AsmFile(const RefPtr<const AsmFile> pparent, uint64_t plineNo, const std::string& pfile)
        : parent(pparent), lineNo(plineNo), file(pfile)
    { }
};

struct AsmMacroSubst: public FastRefCountable
{
    RefPtr<const AsmMacroSubst> parent;   ///< parent macro (null if global scope)
    RefPtr<const AsmFile> file; ///< file where macro substituted
    uint64_t lineNo;  ///< place where macro substituted
    
    AsmMacroSubst(RefPtr<const AsmFile> pfile, uint64_t plineNo)
            : file(pfile), lineNo(plineNo)
    { }
    
    AsmMacroSubst(RefPtr<const AsmMacroSubst> pparent, RefPtr<const AsmFile> pfile,
              size_t plineNo) : parent(pparent), file(pfile), lineNo(plineNo)
    { }
};

struct AsmSourcePos
{
    RefPtr<const AsmFile> file;   ///< file in which message occurred
    RefPtr<const AsmMacroSubst> macro; ///< macro substitution in which message occurred
    
    uint64_t lineNo;
    size_t colNo;
    
    void print(std::ostream& os) const;
};

struct LineTrans
{
    size_t position;
    uint64_t lineNo;
};

struct AsmMacroArg
{
    std::string name;
    std::string defaultValue;
    bool vararg;
    bool required;
};

struct AsmMacro
{
    AsmSourcePos pos;
    uint64_t contentLineNo; //< where is macro content begin
    Array<AsmMacroArg> args;
    std::string content;
    std::vector<LineTrans> colTranslations;
    
    AsmMacro(const AsmSourcePos& pos, uint64_t contentLineNo,
             const Array<AsmMacroArg>& args, const std::string& content);
};

class AsmInputFilter
{
protected:
    size_t pos;
    std::vector<char> buffer;
    std::vector<LineTrans> colTranslations;
    uint64_t lineNo;
    
    AsmInputFilter():  pos(0), lineNo(1)
    { }
public:
    virtual ~AsmInputFilter();
    
    /// read line and returns line except newline character
    virtual const char* readLine(Assembler& assembler, size_t& lineSize) = 0;
    
    /// get current line number before reading line
    uint64_t getLineNo() const
    { return lineNo; }
    
    /// translate position to line number and column number
    /**
     * \param position position in line (from zero)
     */
    LineCol translatePos(size_t position) const;
    
    /// returns column translations
    const std::vector<LineTrans> getColTranslations() const
    { return colTranslations; }
};

/// assembler input layout filter
/** filters input from comments and join splitted lines by backslash.
 * readLine returns prepared line which have only space (' ') and
 * non-space characters. */
class AsmStreamInputFilter: public AsmInputFilter
{
private:
    enum class LineMode: cxbyte
    {
        NORMAL = 0,
        LSTRING,
        STRING,
        LONG_COMMENT,
        LINE_COMMENT
    };
    
    bool managed;
    std::istream* stream;
    LineMode mode;
public:
    explicit AsmStreamInputFilter(std::istream& is);
    explicit AsmStreamInputFilter(const std::string& filename);
    ~AsmStreamInputFilter();
    
    /// read line and returns line except newline character
    const char* readLine(Assembler& assembler, size_t& lineSize);
};

typedef Array<std::pair<std::string, std::string> > AsmMacroArgMap;

class AsmMacroInputFilter: public AsmInputFilter
{
private:
    const AsmMacro& macro;
    AsmMacroArgMap argMap;
    
    const LineTrans* curColTrans;
public:
    AsmMacroInputFilter(const AsmMacro& macro,
        const Array<std::pair<std::string, std::string> >& argMap);
    
    /// read line and returns line except newline character
    const char* readLine(Assembler& assembler, size_t& lineSize);
};

class AsmRepeatInputFilter: public AsmInputFilter
{
private:
    AsmSourcePos repeatPos;
    uint64_t repeatCount;
    uint64_t repeatNum;
    std::vector<LineTrans> repeatColTranslations;
    const LineTrans* curColTrans;
    
public:
    AsmRepeatInputFilter(const AsmSourcePos& pos, uint64_t contentLineNo,
            const std::string& content, uint64_t repeatNum,
            const std::vector<LineTrans>& colTranslations);
    
    /// read line and returns line except newline character
    const char* readLine(Assembler& assembler, size_t& lineSize);
};


class ISAAssembler
{
protected:
    Assembler& assembler;
    explicit ISAAssembler(Assembler& assembler);
public:
    virtual ~ISAAssembler();
    
    virtual size_t assemble(uint64_t lineNo, const char* line,
                std::vector<cxbyte>& output) = 0;
    
    virtual void resolveCode(cxbyte* location, cxbyte targetType, uint64_t value) = 0;
};

class GCNAssembler: public ISAAssembler
{
public:
    explicit GCNAssembler(Assembler& assembler);
    ~GCNAssembler();
    
    size_t assemble(uint64_t lineNo, const char* line, std::vector<cxbyte>& output);
    void resolveCode(cxbyte* location, cxbyte targetType, uint64_t value);
};

/*
 * assembler expressions
 */

enum class AsmExprOp : cxbyte
{
    ARG_VALUE = 0,  ///< is value not operator
    ARG_SYMBOL = 1,  ///< is value not operator
    NEGATE = 2,
    BIT_NOT,
    LOGICAL_NOT,
    PLUS,
    ADDITION,
    SUBTRACT,
    MULTIPLY,
    DIVISION,
    SIGNED_DIVISION,
    MODULO,
    SIGNED_MODULO,
    BIT_AND,
    BIT_OR,
    BIT_XOR,
    BIT_ORNOT,
    SHIFT_LEFT,
    SHIFT_RIGHT,
    SIGNED_SHIFT_RIGHT,
    LOGICAL_AND,
    LOGICAL_OR,
    EQUAL,
    NOT_EQUAL,
    LESS,
    LESS_EQ,
    GREATER,
    GREATER_EQ,
    BELOW, // unsigned less
    BELOW_EQ, // unsigned less or equal
    ABOVE, // unsigned less
    ABOVE_EQ, // unsigned less or equal
    CHOICE,  ///< a ? b : c
    CHOICE_START,
    FIRST_UNARY = NEGATE,
    LAST_UNARY = PLUS,
    FIRST_BINARY = ADDITION,
    LAST_BINARY = ABOVE_EQ,
    NONE = 0xff
};

struct AsmExprTarget;

enum : cxbyte
{
    ASMXTGT_SYMBOL = 0,
    ASMXTGT_DATA8,
    ASMXTGT_DATA16,
    ASMXTGT_DATA32,
    ASMXTGT_DATA64
};

union AsmExprArg;

struct AsmExpression;

struct AsmExprSymbolOccurence
{
    AsmExpression* expression;
    size_t opIndex;
    size_t argIndex;
    
    bool operator==(const AsmExprSymbolOccurence& b) const
    { return expression==b.expression && opIndex==b.opIndex && argIndex==b.argIndex; }
};

struct AsmSymbol
{
    cxuint sectionId;
    bool isDefined;
    bool onceDefined;
    uint64_t value;
    std::vector<AsmSourcePos> occurrences;
    AsmExpression* expression;
    std::vector<AsmExprSymbolOccurence> occurrencesInExprs;
    
    AsmSymbol(bool onceDefined_ = false) : sectionId(ASMSECT_ABS), isDefined(onceDefined_),
            onceDefined(false), value(0), expression(nullptr)
    { }
    AsmSymbol(AsmExpression* expr, bool onceDefined_ = false) :
            sectionId(ASMSECT_ABS), isDefined(onceDefined_),
            value(0), expression(expr)
    { }
    
    AsmSymbol(cxuint inSectionId, uint64_t inValue, bool onceDefined_ = false) :
            sectionId(inSectionId), isDefined(true), onceDefined(onceDefined_),
            value(inValue), expression(nullptr)
    { }
    
    void addOccurrence(AsmSourcePos pos)
    { occurrences.push_back(pos); }
    void addOccurrenceInExpr(AsmExpression* expr, size_t argIndex, size_t opIndex)
    { occurrencesInExprs.push_back({expr, argIndex, opIndex}); }
    void removeOccurrenceInExpr(AsmExpression* expr, size_t argIndex, size_t opIndex);
};

typedef std::unordered_map<std::string, AsmSymbol> AsmSymbolMap;
typedef AsmSymbolMap::value_type AsmSymbolEntry;

struct AsmExprTarget
{
    cxbyte type;
    union
    {
        AsmSymbolEntry* symbol;
        struct {
            cxuint sectionId;
            cxuint size;
            size_t offset;
        };
    };
};

struct AsmExpression
{
    AsmExprTarget target;
    AsmSourcePos sourcePos;
    size_t symOccursNum;
    Array<AsmExprOp> ops;
    std::unique_ptr<LineCol[]> messagePositions;    ///< for every potential message
    std::unique_ptr<AsmExprArg[]> args;
    
    AsmSourcePos getSourcePos(size_t msgPosIndex) const
    {
        AsmSourcePos pos = sourcePos;
        pos.lineNo = messagePositions[msgPosIndex].lineNo;
        pos.colNo = messagePositions[msgPosIndex].colNo;
        return pos;
    }
    
    AsmExpression(const AsmSourcePos& pos, size_t symOccursNum,
            size_t opsNum, size_t opPosNum, size_t argsNum);
    AsmExpression(const AsmSourcePos& pos, size_t symOccursNum,
              size_t opsNum, const AsmExprOp* ops, size_t opPosNum,
              const LineCol* opPos, size_t argsNum, const AsmExprArg* args);
    ~AsmExpression() = default;
    
    bool evaluate(Assembler& assembler, uint64_t& value) const;
    
    static AsmExpression* parse(Assembler& assembler, size_t linePos, size_t& outLinePos);
    
    static AsmExpression* parse(Assembler& assembler, const char* string,
              const char*& outend);
    
    static bool isUnaryOp(AsmExprOp op)
    { return (AsmExprOp::FIRST_UNARY <= op && op <= AsmExprOp::LAST_UNARY); }
    
    static bool isBinaryOp(AsmExprOp op)
    { return (AsmExprOp::FIRST_BINARY <= op && op <= AsmExprOp::LAST_BINARY); }
};

union AsmExprArg
{
    AsmSymbolEntry* symbol;
    uint64_t value;
};

struct AsmSection
{
    cxuint kernelId;
    AsmSectionType type;
    std::vector<cxbyte> content;
};

struct AsmCondClause
{
    RefPtr<AsmMacroSubst> macroSubst;
    std::vector<std::pair<AsmSourcePos, uint64_t> > positions;
};

class Assembler
{
public:
    typedef std::pair<std::string, uint64_t> DefSym;
    typedef std::unordered_map<std::string, AsmMacro> MacroMap;
    typedef std::unordered_map<std::string, cxuint> KernelMap;
private:
    friend class AsmStreamInputFilter;
    friend class AsmMacroInputFilter;
    friend struct AsmExpression;
    AsmFormat format;
    GPUDeviceType deviceType;
    ISAAssembler* isaAssembler;
    std::vector<DefSym> defSyms;
    std::vector<std::string> includeDirs;
    std::vector<AsmSection> sections;
    AsmSymbolMap symbolMap;
    MacroMap macroMap;
    KernelMap kernelMap;
    cxuint flags;
    uint64_t macroCount;
    
    cxuint inclusionLevel;
    cxuint macroSubstLevel;
    RefPtr<const AsmFile> topFile;
    RefPtr<const AsmMacroSubst> topMacroSubst;
    
    size_t lineSize;
    const char* line;
    uint64_t lineNo;
    
    std::stack<AsmInputFilter*> asmInputFilters;
    AsmInputFilter* currentInputFilter;
    
    std::ostream& messageStream;
    
    union {
        AmdInput* amdOutput;
        GalliumInput* galliumOutput;
    };
    
    std::stack<AsmCondClause> condClauses;
    
    bool inGlobal;
    bool inAmdConfig;
    cxuint currentKernel;
    cxuint currentSection;
    uint64_t& currentOutPos;
    
    void printWarning(const AsmSourcePos& pos, const char* message);
    void printError(const AsmSourcePos& pos, const char* message);
    
    void printWarning(const char* linePlace, const char* message)
    {
        const LineCol lineCol = currentInputFilter->translatePos(linePlace-line);
        printWarning({ topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo }, message);
    }
    void printError(const char* linePlace, const char* message)
    {
        const LineCol lineCol = currentInputFilter->translatePos(linePlace-line);
        printError({ topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo }, message);
    }
    
    void printWarning(LineCol lineCol, const char* message)
    { printWarning({ topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo }, message); }
    void printError(LineCol lineCol, const char* message)
    { printError({ topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo }, message); }
    
    uint64_t parseLiteral(const char* string, const char*& outend);
    AsmSymbolEntry* parseSymbol(const char* string, bool localLabel = true);
    
    LineCol translatePos(const char* string) const
    { return currentInputFilter->translatePos(string-line); }
    LineCol translatePos(size_t pos) const
    { return currentInputFilter->translatePos(pos); }
    
    AsmSourcePos getSourcePos(const char* string) const
    {
        LineCol lineCol = translatePos(string);
        return { topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo };
    }
    AsmSourcePos getSourcePos(size_t pos) const
    {
        LineCol lineCol = translatePos(pos);
        return { topFile, topMacroSubst, lineCol.lineNo, lineCol.colNo };
    }
    
    void includeFile(const std::string& filename);
    void applyMacro(const std::string& macroName, AsmMacroArgMap argMap);
    void exitFromMacro();
    
    bool setSymbol(AsmSymbolEntry& symEntry, uint64_t value);
    
    bool assignSymbol(const std::string& symbolName, const char* stringAtSymbol,
                  const char* string);
protected:    
    void readLine();
public:
    explicit Assembler(const std::string& filename, std::istream& input, cxuint flags = 0,
              std::ostream& msgStream = std::cerr);
    ~Assembler();
    
    GPUDeviceType getDeviceType() const
    { return deviceType; }
    
    void setDeviceType(const GPUDeviceType deviceType)
    { this->deviceType = deviceType; }
    
    cxuint getFlags() const
    { return flags; }
    void setFlags(cxuint flags)
    { this->flags = flags; }
    
    const std::vector<std::string>& getIncludeDirs() const
    { return includeDirs; }
    
    void addIncludeDir(const std::string& includeDir);
    
    const AsmSymbolMap& getSymbolMap() const
    { return symbolMap; }
    
    void addInitialDefSym(const std::string& symName, uint64_t name);
    
    const AmdInput* getAmdOutput() const
    { return amdOutput; }
    
    const GalliumInput* getGalliumOutput() const
    { return galliumOutput; }
    
    void assemble();
};

}

#endif
