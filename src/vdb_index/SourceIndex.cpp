// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Static source index of an elaborated design
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2026 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "SourceIndex.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/driver/Driver.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/Json.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Hash.h"
#include "slang/util/VersionInfo.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace vdb_index {

using namespace slang;
using namespace slang::ast;
using namespace slang::syntax;
using SToken = slang::parsing::Token;

namespace {

const std::vector<std::string_view> CLASS_NAMES = {
    "keyword",  "comment",   "number",     "string",   "operator",  "macro",
    "variable", "parameter", "enumMember", "type",     "module",    "interface",
    "package",  "instance",  "function",   "property", "namespace",
};
const std::vector<std::string_view> MODIFIER_NAMES = {
    "declaration", "input",    "output",         "inout",    "ref",
    "clock",       "readonly", "defaultLibrary", "argument",
};

uint32_t mod(Modifier m) { return static_cast<uint32_t>(m); }

uint32_t directionModifier(ArgumentDirection direction) {
    switch (direction) {
    case ArgumentDirection::In: return mod(Modifier::Input);
    case ArgumentDirection::Out: return mod(Modifier::Output);
    case ArgumentDirection::InOut: return mod(Modifier::InOut);
    case ArgumentDirection::Ref: return mod(Modifier::Ref);
    }
    return 0;
}

struct Classification {
    TokenClass cls;
    uint32_t modifiers;
};

// Maps a symbol to the class and modifiers of the tokens that denote it.
std::optional<Classification> classifySymbol(const Symbol& symbol) {
    using enum SymbolKind;
    switch (symbol.kind) {
    case Parameter:
    case TypeParameter:
    case Genvar:
    case Specparam:
    case DefParam: return Classification{TokenClass::Parameter, mod(Modifier::Readonly)};
    case EnumValue: return Classification{TokenClass::EnumMember, mod(Modifier::Readonly)};
    case Variable:
    case Net:
    case ClockVar:
    case LocalAssertionVar:
    case Iterator:
    case PatternVar: {
        uint32_t mods = 0;
        if (const auto* backref = symbol.as<ValueSymbol>().getFirstPortBackref()) {
            mods |= directionModifier(backref->port->direction);
        }
        return Classification{TokenClass::Variable, mods};
    }
    case FormalArgument:
        return Classification{
            TokenClass::Variable,
            mod(Modifier::Argument)
                | directionModifier(symbol.as<FormalArgumentSymbol>().direction)};
    case Port:
        return Classification{TokenClass::Variable,
                              directionModifier(symbol.as<PortSymbol>().direction)};
    case MultiPort:
        return Classification{TokenClass::Variable,
                              directionModifier(symbol.as<MultiPortSymbol>().direction)};
    case ModportPort:
        return Classification{TokenClass::Variable,
                              directionModifier(symbol.as<ModportPortSymbol>().direction)};
    case InterfacePort:
    case Instance:
    case InstanceArray:
    case PrimitiveInstance:
    case CheckerInstance: return Classification{TokenClass::Instance, 0};
    case Modport:
    case ModportClocking: return Classification{TokenClass::Interface, 0};
    case Definition:
        return Classification{symbol.as<DefinitionSymbol>().definitionKind
                                      == DefinitionKind::Interface
                                  ? TokenClass::Interface
                                  : TokenClass::Module,
                              0};
    case Package: return Classification{TokenClass::Package, 0};
    case TypeAlias:
    case ForwardingTypedef:
    case NetType:
    case GenericClassDef:
    case PredefinedIntegerType:
    case ScalarType:
    case FloatingType:
    case EnumType:
    case PackedArrayType:
    case FixedSizeUnpackedArrayType:
    case DynamicArrayType:
    case DPIOpenArrayType:
    case AssociativeArrayType:
    case QueueType:
    case PackedStructType:
    case UnpackedStructType:
    case PackedUnionType:
    case UnpackedUnionType:
    case ClassType:
    case CovergroupType:
    case VoidType:
    case NullType:
    case CHandleType:
    case StringType:
    case EventType:
    case VirtualInterfaceType: return Classification{TokenClass::Type, 0};
    case Subroutine:
    case MethodPrototype:
    case LetDecl: return Classification{TokenClass::Function, 0};
    case Field:
    case ClassProperty: return Classification{TokenClass::Property, 0};
    case GenerateBlock:
    case GenerateBlockArray:
    case StatementBlock:
    case ProceduralBlock:
    case ClockingBlock:
    case Sequence:
    case Property:
    case Checker:
    case CovergroupBody:
    case Coverpoint:
    case CoverCross:
    case ConstraintBlock:
    case CompilationUnit: return Classification{TokenClass::Namespace, 0};
    default: return std::nullopt;
    }
}

bool isLiteral(parsing::TokenKind kind) {
    using enum parsing::TokenKind;
    switch (kind) {
    case IntegerLiteral:
    case IntegerBase:
    case UnbasedUnsizedLiteral:
    case RealLiteral:
    case TimeLiteral: return true;
    default: return false;
    }
}

bool isMacroToken(parsing::TokenKind kind) {
    using enum parsing::TokenKind;
    switch (kind) {
    case Directive:
    case MacroUsage:
    case MacroQuote:
    case MacroTripleQuote:
    case MacroEscapedQuote:
    case MacroPaste:
    case EmptyMacroArgument: return true;
    default: return false;
    }
}

bool hasRawText(parsing::TriviaKind kind) {
    using enum parsing::TriviaKind;
    switch (kind) {
    case Whitespace:
    case EndOfLine:
    case LineComment:
    case BlockComment:
    case DisabledText: return true;
    default: return false;
    }
}

bool isCommentLike(parsing::TriviaKind kind) {
    using enum parsing::TriviaKind;
    return kind == LineComment || kind == BlockComment || kind == DisabledText;
}

// What the elaborated design says about one identifier token.
struct Semantic {
    TokenClass cls;
    uint32_t modifiers;
    SourceLocation declaration;
};

// Leaf identifier tokens of a name expression, left to right.
void nameTokens(const SyntaxNode* node, SmallVectorBase<SToken>& out) {
    if (!node) return;
    switch (node->kind) {
    case SyntaxKind::IdentifierName:
        out.push_back(node->as<IdentifierNameSyntax>().identifier);
        break;
    case SyntaxKind::IdentifierSelectName:
        out.push_back(node->as<IdentifierSelectNameSyntax>().identifier);
        break;
    case SyntaxKind::ClassName: out.push_back(node->as<ClassNameSyntax>().identifier); break;
    case SyntaxKind::ScopedName: {
        const auto& scoped = node->as<ScopedNameSyntax>();
        nameTokens(scoped.left, out);
        nameTokens(scoped.right, out);
        break;
    }
    case SyntaxKind::ElementSelectExpression:
        nameTokens(node->as<ElementSelectExpressionSyntax>().left, out);
        break;
    case SyntaxKind::MemberAccessExpression: {
        const auto& access = node->as<MemberAccessExpressionSyntax>();
        nameTokens(access.left, out);
        out.push_back(access.name);
        break;
    }
    case SyntaxKind::InvocationExpression:
        nameTokens(node->as<InvocationExpressionSyntax>().left, out);
        break;
    default: break;
    }
}

// Collects, over the whole elaborated design, what each identifier token denotes.
struct Semantics {
    const SourceManager& sm;
    Compilation& compilation;
    flat_hash_map<SourceLocation, Semantic> byToken;
    flat_hash_set<SourceLocation> clocks;

    bool inFile(SourceLocation loc) const { return loc.valid() && !sm.isMacroLoc(loc); }

    void record(SourceLocation token, const Symbol& target, bool declaration) {
        if (!inFile(token)) return;
        auto classification = classifySymbol(target);
        if (!classification) return;
        uint32_t mods = classification->modifiers;
        if (declaration) mods |= mod(Modifier::Declaration);
        auto [it, inserted]
            = byToken.try_emplace(token, Semantic{classification->cls, mods, target.location});
        if (!inserted) it->second.modifiers |= mods;
    }

    void declare(const Symbol& symbol) { record(symbol.location, symbol, true); }
    void reference(SToken token, const Symbol& target) {
        if (token) record(token.location(), target, target.location == token.location());
    }

    // `pkg::name`: the package prefix, when the name is scoped.
    void packagePrefix(const SyntaxNode* syntax) {
        if (!syntax || syntax->kind != SyntaxKind::ScopedName) return;
        const auto& scoped = syntax->as<ScopedNameSyntax>();
        if (scoped.left->kind != SyntaxKind::IdentifierName) return;
        const SToken token = scoped.left->as<IdentifierNameSyntax>().identifier;
        if (const auto* package = compilation.getPackage(token.valueText())) {
            reference(token, *package);
        }
    }

    // Named type references in a declared type: `my_t x;` or `pkg::my_t x;`.
    void declaredType(const DeclaredType& declared) {
        const auto* syntax = declared.getTypeSyntax();
        if (!syntax || syntax->kind != SyntaxKind::NamedType) return;
        const auto& named = syntax->as<NamedTypeSyntax>();
        SmallVector<SToken> tokens;
        nameTokens(named.name, tokens);
        if (tokens.empty()) return;
        packagePrefix(named.name);
        const Type& type = declared.getType();
        if (type.kind == SymbolKind::TypeAlias || type.kind == SymbolKind::TypeParameter) {
            reference(tokens.back(), type);
        }
    }

    // Sub-expressions of a name chain (`a.b`, `u.q[0]`) carry no syntax, only a
    // source range that ends with the identifier they denote.
    std::optional<SourceLocation> nameBefore(SourceLocation end, std::string_view name) const {
        if (!end.valid() || sm.isMacroLoc(end) || name.empty()) return std::nullopt;
        const std::string_view text = sm.getSourceText(end.buffer());
        const size_t offset = end.offset();
        if (offset < name.size() || offset > text.size()) return std::nullopt;
        if (text.substr(offset - name.size(), name.size()) != name) return std::nullopt;
        return SourceLocation{end.buffer(), offset - name.size()};
    }

    std::optional<SourceLocation> referenceEnd(SourceRange range, const Symbol& target) {
        const auto loc = nameBefore(range.end(), target.name);
        if (loc) record(*loc, target, target.location == *loc);
        return loc;
    }

    static bool isIdentChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
               || c == '_' || c == '$';
    }

    // The identifier ending at `pos` after optional whitespace, or empty.
    static std::string_view identifierBefore(std::string_view text, size_t& pos) {
        while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) --pos;
        size_t start = pos;
        while (start > 0 && isIdentChar(text[start - 1])) --start;
        const std::string_view ident = text.substr(start, pos - start);
        pos = start;
        return ident;
    }

    // `pkg::name` written in the source: the package before the name at `loc`.
    void packagePrefixAt(SourceLocation loc) {
        const std::string_view text = sm.getSourceText(loc.buffer());
        size_t pos = loc.offset();
        while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) --pos;
        if (pos < 2 || text.substr(pos - 2, 2) != "::") return;
        pos -= 2;
        const std::string_view name = identifierBefore(text, pos);
        if (name.empty()) return;
        if (const auto* package = compilation.getPackage(name)) {
            record(SourceLocation{loc.buffer(), pos}, *package, false);
        }
    }

    // `a.b[i].c` written in the source: the path elements before the name at `loc`,
    // matched by name from the right.
    void hierarchicalPrefixAt(SourceLocation loc, const HierarchicalReference& ref) {
        const std::string_view text = sm.getSourceText(loc.buffer());
        size_t pos = loc.offset();
        size_t element = ref.path.size();
        while (element > 0) {
            while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) --pos;
            if (pos == 0 || text[pos - 1] != '.') return;
            --pos;
            while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) --pos;
            while (pos > 0 && text[pos - 1] == ']') {
                int depth = 0;
                do {
                    --pos;
                    if (text[pos] == ']') ++depth;
                    if (text[pos] == '[') --depth;
                } while (pos > 0 && depth > 0);
                while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) --pos;
            }
            const std::string_view name = identifierBefore(text, pos);
            if (name.empty()) return;
            bool matched = false;
            while (element > 0) {
                --element;
                const Symbol& symbol = *ref.path[element].symbol;
                if (symbol.name == name) {
                    record(SourceLocation{loc.buffer(), pos}, symbol, false);
                    matched = true;
                    break;
                }
            }
            if (!matched) return;
        }
    }
};

// Finds edge-sensitive event signals a process never reads as data: the clocks.
struct ReferenceCollector : public ASTVisitor<ReferenceCollector, VisitFlags::AllGood> {
    flat_hash_set<SourceLocation> refs;
    void handle(const ValueExpressionBase& expr) { refs.insert(expr.symbol.location); }
};

void collectEdges(const TimingControl& timing, flat_hash_set<SourceLocation>& out) {
    switch (timing.kind) {
    case TimingControlKind::SignalEvent: {
        const auto& event = timing.as<SignalEventControl>();
        if (event.edge == EdgeKind::None) return;
        if (const auto* symbol = event.expr.getSymbolReference()) out.insert(symbol->location);
        return;
    }
    case TimingControlKind::EventList:
        for (const auto* event : timing.as<EventListControl>().events) collectEdges(*event, out);
        return;
    default: return;
    }
}

void findClocks(const ProceduralBlockSymbol& block, flat_hash_set<SourceLocation>& clocks) {
    const Statement& body = block.getBody();
    if (body.kind != StatementKind::Timed) return;
    const auto& timed = body.as<TimedStatement>();
    flat_hash_set<SourceLocation> events;
    collectEdges(timed.timing, events);
    if (events.empty()) return;
    ReferenceCollector reads;
    timed.stmt.visit(reads);
    for (const auto& loc : events) {
        if (!reads.refs.contains(loc)) clocks.insert(loc);
    }
}

// Walks every elaborated instance once per distinct parameterization and
// records declarations and references.
struct DesignVisitor : public ASTVisitor<DesignVisitor, VisitFlags::AllGood> {
    Semantics& sem;
    flat_hash_set<std::string> visitedBodies;

    explicit DesignVisitor(Semantics& semantics)
        : sem{semantics} {}

    template <typename T>
    void handle(const T& node) {
        if constexpr (std::is_same_v<T, InstanceSymbol>) {
            // Top-level instances share the module name token with the definition.
            if (node.getSyntax() && node.getSyntax()->kind == SyntaxKind::HierarchicalInstance) {
                sem.declare(node);
            }
        } else if constexpr (std::is_base_of_v<Symbol, T>) {
            sem.declare(node);
        }
        if constexpr (std::is_base_of_v<ValueSymbol, T>) sem.declaredType(*node.getDeclaredType());
        if constexpr (std::is_same_v<T, TypeAliasType>) sem.declaredType(node.targetType);
        if constexpr (std::is_same_v<T, ProceduralBlockSymbol>) findClocks(node, sem.clocks);
        if constexpr (std::is_same_v<T, WildcardImportSymbol>
                      || std::is_same_v<T, ExplicitImportSymbol>) {
            imports(node);
        }
        if constexpr (std::is_same_v<T, InterfacePortSymbol>) interfacePort(node);
        if constexpr (std::is_same_v<T, InstanceSymbol>) {
            instantiation(node);
            if (!visitedBodies.insert(bodyKey(node)).second) return;
        }
        if constexpr (std::is_same_v<T, NamedValueExpression>) {
            if (const auto loc = sem.referenceEnd(node.sourceRange, node.symbol)) {
                sem.packagePrefixAt(*loc);
            }
        }
        if constexpr (std::is_same_v<T, HierarchicalValueExpression>) {
            if (const auto loc = sem.referenceEnd(node.sourceRange, node.symbol)) {
                sem.hierarchicalPrefixAt(*loc, node.ref);
            }
        }
        if constexpr (std::is_same_v<T, ArbitrarySymbolExpression>) {
            if (const auto loc = sem.referenceEnd(node.sourceRange, *node.symbol)) {
                sem.packagePrefixAt(*loc);
            }
        }
        if constexpr (std::is_same_v<T, MemberAccessExpression>) {
            sem.referenceEnd(node.sourceRange, node.member);
        }
        if constexpr (std::is_same_v<T, CallExpression>) call(node);
        visitDefault(node);
    }

    static std::string bodyKey(const InstanceSymbol& inst) {
        std::string key = std::to_string(reinterpret_cast<uintptr_t>(&inst.getDefinition()));
        for (const auto* param : inst.body.getParameters()) {
            key += '|';
            if (param->symbol.kind == SymbolKind::Parameter) {
                key += param->symbol.as<ParameterSymbol>().getValue().toString();
            } else if (param->symbol.kind == SymbolKind::TypeParameter) {
                key += param->symbol.as<TypeParameterSymbol>().targetType.getType().toString();
            }
        }
        return key;
    }

    void imports(const Symbol& symbol) {
        const auto* syntax = symbol.getSyntax();
        if (!syntax || syntax->kind != SyntaxKind::PackageImportItem) return;
        const auto& item = syntax->as<PackageImportItemSyntax>();
        if (const auto* package = sem.compilation.getPackage(item.package.valueText())) {
            sem.reference(item.package, *package);
        }
        if (symbol.kind == SymbolKind::ExplicitImport) {
            if (const auto* imported = symbol.as<ExplicitImportSymbol>().importedSymbol()) {
                sem.reference(item.item, *imported);
            }
        }
    }

    void interfacePort(const InterfacePortSymbol& port) {
        const auto* syntax = port.getSyntax();
        if (!syntax || syntax->kind != SyntaxKind::ImplicitAnsiPort) return;
        const SyntaxNode* header = syntax->as<ImplicitAnsiPortSyntax>().header;
        if (!header || header->kind != SyntaxKind::InterfacePortHeader || !port.interfaceDef)
            return;
        const auto& iface = header->as<InterfacePortHeaderSyntax>();
        sem.reference(iface.nameOrKeyword, *port.interfaceDef);
    }

    // The module name, parameter overrides and port names of an instantiation.
    void instantiation(const InstanceSymbol& inst) {
        const auto* syntax = inst.getSyntax();
        if (!syntax || syntax->kind != SyntaxKind::HierarchicalInstance) return;
        const SyntaxNode* parent = syntax->parent;
        if (!parent || parent->kind != SyntaxKind::HierarchyInstantiation) return;
        const auto& instantiation = parent->as<HierarchyInstantiationSyntax>();
        sem.reference(instantiation.type, inst.getDefinition());
        if (instantiation.parameters) {
            for (const auto* assignment : instantiation.parameters->parameters) {
                if (assignment->kind != SyntaxKind::NamedParamAssignment) continue;
                const SToken name = assignment->as<NamedParamAssignmentSyntax>().name;
                for (const auto* param : inst.body.getParameters()) {
                    if (param->symbol.name == name.valueText()) {
                        sem.reference(name, param->symbol);
                        break;
                    }
                }
            }
        }
        for (const auto* connection : syntax->as<HierarchicalInstanceSyntax>().connections) {
            if (connection->kind != SyntaxKind::NamedPortConnection) continue;
            const SToken name = connection->as<NamedPortConnectionSyntax>().name;
            for (const auto* port : inst.body.getPortList()) {
                if (port->name == name.valueText()) {
                    sem.reference(name, *port);
                    break;
                }
            }
        }
    }

    void call(const CallExpression& expr) {
        if (expr.isSystemCall()) return;
        const auto* subroutine = std::get<0>(expr.subroutine);
        if (!subroutine) return;
        SmallVector<SToken> tokens;
        nameTokens(expr.syntax, tokens);
        if (!tokens.empty()) sem.reference(tokens.back(), *subroutine);
        sem.packagePrefix(expr.syntax && expr.syntax->kind == SyntaxKind::InvocationExpression
                              ? expr.syntax->as<InvocationExpressionSyntax>().left
                              : expr.syntax);
    }
};

// Generate blocks an instance leaves uninstantiated, without descending into
// child instances (they are reported under their own paths).
void inactiveBlocks(const Scope& scope, SmallVectorBase<const GenerateBlockSymbol*>& out) {
    for (const auto& member : scope.members()) {
        if (member.kind == SymbolKind::GenerateBlockArray) {
            inactiveBlocks(member.as<GenerateBlockArraySymbol>(), out);
        } else if (member.kind == SymbolKind::GenerateBlock) {
            const auto& block = member.as<GenerateBlockSymbol>();
            if (block.isUninstantiated) {
                out.push_back(&block);
            } else {
                inactiveBlocks(block, out);
            }
        }
    }
}

struct InstanceVisitor : public ASTVisitor<InstanceVisitor, VisitFlags::Symbols> {
    std::vector<std::pair<std::string, SmallVector<const GenerateBlockSymbol*>>> found;
    void handle(const InstanceSymbol& inst) {
        SmallVector<const GenerateBlockSymbol*> blocks;
        inactiveBlocks(inst.body, blocks);
        if (!blocks.empty()) found.emplace_back(inst.getHierarchicalPath(), std::move(blocks));
        visitDefault(inst);
    }
};

// Turns the token stream of every buffer into classified tokens.
class Builder {
public:
    Builder(const SourceManager& sm, Semantics& sem)
        : m_sm{sm}
        , m_sem{sem} {}

    struct File {
        BufferID buffer;
        std::vector<Token> tokens;
        size_t previousEnd = 0;
    };

    std::vector<File>& files() { return m_files; }

    // Files are registered up front, in load order, so declaration locations
    // resolve regardless of which file is lexed first.
    void registerBuffer(BufferID buffer) {
        if (m_index.try_emplace(buffer, static_cast<int32_t>(m_files.size())).second) {
            m_files.push_back(File{buffer, {}, 0});
            if (const SourceLocation from = m_sm.getIncludedFrom(buffer); from.valid()) {
                m_includes.emplace_back(from, buffer);
            }
        }
    }
    int32_t fileIndex(BufferID buffer) const {
        const auto it = m_index.find(buffer);
        return it == m_index.end() ? -1 : it->second;
    }

    Location locate(SourceLocation loc) const {
        if (!loc.valid() || m_sm.isMacroLoc(loc)) return {};
        const int32_t file = fileIndex(loc.buffer());
        if (file < 0) return {};
        return Location{file, static_cast<uint32_t>(m_sm.getLineNumber(loc)),
                        static_cast<uint32_t>(m_sm.getColumnNumber(loc))};
    }

    void token(SToken token) {
        SourceLocation loc = token.location();
        if (!loc.valid()) return;
        // Macro argument tokens keep the text the user wrote; expansions do not.
        while (m_sm.isMacroArgLoc(loc)) loc = m_sm.getOriginalLoc(loc);
        if (m_sm.isMacroLoc(loc)) {
            expansion(loc);
            return;
        }
        const int32_t file = fileIndex(loc.buffer());
        if (file < 0) return;
        if (loc == token.location()) {
            File& current = m_files[static_cast<size_t>(file)];
            comments(token, SourceLocation{current.buffer, current.previousEnd});
            current.previousEnd = loc.offset() + token.rawText().size();
        }
        classify(token, loc, false);
    }

private:
    using Sink = std::vector<std::pair<int32_t, Token>>;

    const SourceManager& m_sm;
    Semantics& m_sem;
    std::vector<File> m_files;
    flat_hash_map<BufferID, int32_t> m_index;
    flat_hash_set<SourceLocation> m_expansions;
    std::vector<std::pair<SourceLocation, BufferID>> m_includes;  // directive -> file

    // The buffer an include directive spanning [start, end) pulled in, if any.
    std::optional<BufferID> includedBuffer(SourceLocation start, SourceLocation end) const {
        for (const auto& [from, buffer] : m_includes) {
            if (from.buffer() == start.buffer() && from.offset() >= start.offset()
                && from.offset() < end.offset()) {
                return buffer;
            }
        }
        return std::nullopt;
    }

    // Appends one token per line covered by `text` starting at `loc`.
    void add(SourceLocation loc, std::string_view text, TokenClass cls, uint32_t modifiers,
             Location declaration = {}, Sink* sink = nullptr) {
        const int32_t file = fileIndex(loc.buffer());
        if (file < 0) return;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            size_t length = end - start;
            if (length > 0 && text[start + length - 1] == '\r') --length;
            if (length > 0) {
                const SourceLocation at = loc + start;
                const Token token{static_cast<uint32_t>(m_sm.getLineNumber(at)),
                                  static_cast<uint32_t>(m_sm.getColumnNumber(at)),
                                  static_cast<uint32_t>(length),
                                  cls,
                                  modifiers,
                                  declaration};
                if (sink) {
                    sink->emplace_back(file, token);
                } else {
                    m_files[static_cast<size_t>(file)].tokens.push_back(token);
                }
            }
            start = end + 1;
        }
    }

    void classify(SToken token, SourceLocation loc, bool directive) {
        const std::string_view text = token.rawText();
        if (token.isMissing() || text.empty()) return;
        using enum parsing::TokenKind;
        std::optional<Classification> classification;
        Location declaration;
        if (parsing::LexerFacts::isKeyword(token.kind)) {
            classification = Classification{TokenClass::Keyword, 0};
        } else if (token.kind == Identifier) {
            if (directive) {
                classification = Classification{TokenClass::Macro, 0};
            } else if (const auto it = m_sem.byToken.find(token.location());
                       it != m_sem.byToken.end()) {
                const Semantic& semantic = it->second;
                uint32_t mods = semantic.modifiers;
                if (m_sem.clocks.contains(semantic.declaration)) mods |= mod(Modifier::Clock);
                classification = Classification{semantic.cls, mods};
                declaration = locate(semantic.declaration);
            }
        } else if (token.kind == SystemIdentifier) {
            classification = Classification{TokenClass::Function, mod(Modifier::DefaultLibrary)};
        } else if (token.kind == StringLiteral || token.kind == IncludeFileName) {
            classification = Classification{TokenClass::String, 0};
        } else if (isLiteral(token.kind)) {
            classification = Classification{TokenClass::Number, 0};
        } else if (isMacroToken(token.kind)) {
            classification = Classification{TokenClass::Macro, 0};
        } else if (token.kind != EndOfFile && token.kind != Unknown) {
            classification = Classification{TokenClass::Operator, 0};
        }
        if (classification) {
            add(loc, text, classification->cls, classification->modifiers, declaration);
        }
    }

    // A macro expansion is shown as the macro usage the user wrote.
    void expansion(SourceLocation loc) {
        SourceRange range = m_sm.getExpansionRange(loc);
        while (range.start().valid() && m_sm.isMacroLoc(range.start())) {
            range = m_sm.getExpansionRange(range.start());
        }
        if (!range.start().valid() || !m_expansions.insert(range.start()).second) return;
        const std::string_view text = m_sm.getSourceText(range.start().buffer());
        const size_t start = range.start().offset();
        const size_t end = std::min(range.end().offset(), text.size());
        if (end > start) add(range.start(), text.substr(start, end - start), TokenClass::Macro, 0);
    }

    // Tokens of a preprocessor directive are trivia of the token that follows;
    // the directive's own leading comments are trivia of its first token, laid
    // out from `start` when the directive carries an explicit location.
    void directiveTokens(const SyntaxNode& syntax, std::optional<SourceLocation> start) {
        struct Visitor : public SyntaxVisitor<Visitor> {
            Builder& builder;
            std::optional<SourceLocation> cursor;
            Visitor(Builder& b, std::optional<SourceLocation> c)
                : builder{b}
                , cursor{c} {}
            void visitToken(SToken token) {
                const SourceLocation loc = token.location();
                if (!loc.valid() || builder.m_sm.isMacroLoc(loc)) return;
                if (cursor && cursor->buffer() == loc.buffer()) builder.comments(token, *cursor);
                cursor = loc + token.rawText().size();
                builder.classify(token, loc, true);
            }
        };
        Visitor visitor{*this, start};
        syntax.visit(visitor);
    }

    // Where a directive or skipped-token trivia ends, if that is a file location.
    std::optional<SourceLocation> triviaEnd(const parsing::Trivia& trivia) const {
        SToken last;
        if (trivia.kind == parsing::TriviaKind::Directive) {
            last = trivia.syntax()->getLastToken();
        } else if (trivia.kind == parsing::TriviaKind::SkippedTokens) {
            const auto skipped = trivia.getSkippedTokens();
            if (skipped.empty()) return std::nullopt;
            last = skipped.back();
        } else {
            return std::nullopt;
        }
        const SourceLocation loc = last.location();
        if (!loc.valid() || m_sm.isMacroLoc(loc)) return std::nullopt;
        return loc + last.rawText().size();
    }

    // Emits the comments preceding `token`. Plain trivia carry no locations, so
    // they are laid out forward from the previous token's end, following
    // directives and explicit locations into other buffers; when that walk does
    // not arrive exactly at the token, the trailing run of plain trivia is laid
    // out backward from the token instead.
    void comments(SToken token, SourceLocation cursor) {
        const auto trivia = token.trivia();
        if (trivia.empty()) return;
        const SourceLocation target = token.location();
        Sink pending;
        std::vector<SourceLocation> resume;  // where to continue after an included file
        bool consistent = true;
        for (const auto& item : trivia) {
            std::optional<SourceLocation> explicitLoc = item.getExplicitLocation();
            if (explicitLoc && (!explicitLoc->valid() || m_sm.isMacroLoc(*explicitLoc))) {
                explicitLoc.reset();
            }
            if (item.kind == parsing::TriviaKind::Directive) {
                directiveTokens(*item.syntax(), explicitLoc);
            }
            if (explicitLoc) cursor = *explicitLoc;
            if (hasRawText(item.kind)) {
                if (isCommentLike(item.kind)) {
                    add(cursor, item.getRawText(), TokenClass::Comment, 0, {}, &pending);
                }
                cursor = cursor + item.getRawText().size();
                if (!resume.empty()
                    && cursor.offset() >= m_sm.getSourceText(cursor.buffer()).size()) {
                    cursor = resume.back();
                    resume.pop_back();
                }
            } else if (const auto end = triviaEnd(item)) {
                cursor = *end;
                if (item.syntax()->kind == SyntaxKind::IncludeDirective) {
                    const SourceLocation start = item.syntax()->getFirstToken().location();
                    if (const auto included = includedBuffer(start, *end)) {
                        resume.push_back(cursor);
                        cursor = SourceLocation{*included, 0};
                    }
                }
            } else {
                consistent = false;
                break;
            }
        }
        if (consistent && cursor == target) {
            for (const auto& [index, pendingToken] : pending) {
                m_files[static_cast<size_t>(index)].tokens.push_back(pendingToken);
            }
            return;
        }
        size_t end = target.offset();
        for (auto it = trivia.rbegin(); it != trivia.rend(); ++it) {
            if (!hasRawText(it->kind)) break;
            const std::string_view text = it->getRawText();
            if (text.size() > end) break;
            end -= text.size();
            if (isCommentLike(it->kind)) {
                add(SourceLocation{target.buffer(), end}, text, TokenClass::Comment, 0);
            }
        }
    }
};

struct LexicalVisitor : public SyntaxVisitor<LexicalVisitor> {
    Builder& builder;
    explicit LexicalVisitor(Builder& b)
        : builder{b} {}
    void visitToken(SToken token) { builder.token(token); }
};

std::filesystem::path canonicalOrSelf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : result;
}

std::vector<std::string> arguments(const IndexRequest& request) {
    const Elaboration& e = request.elaboration;
    std::vector<std::string> args{"verilator_vdb_index"};
    // Verilator elaborates all files as one unit, tolerates unknown modules and
    // resolves references before their declaration in the file order it was given.
    args.emplace_back("--single-unit");
    args.emplace_back("--ignore-unknown-modules");
    args.emplace_back("--allow-use-before-declare");
    args.emplace_back("-Wnone");
    if (e.language.rfind("1800-", 0) == 0) {
        args.emplace_back("--std");
        args.emplace_back(e.language);
    }
    if (!request.top.empty()) {
        args.emplace_back("--top");
        args.emplace_back(request.top);
    }
    for (const auto& dir : e.includeDirs) {
        args.emplace_back("-I");
        args.emplace_back(dir);
    }
    bool libExts = false;
    for (const auto& ext : e.libraryExts) {
        if (ext.empty()) continue;
        args.emplace_back("--libext");
        args.emplace_back(ext);
        libExts = true;
    }
    if (libExts) {
        for (const auto& dir : e.includeDirs) {
            args.emplace_back("-y");
            args.emplace_back(dir);
        }
    }
    for (const auto& [name, value] : e.defines) {
        args.emplace_back("-D");
        args.emplace_back(value.empty() ? name : name + "=" + value);
    }
    for (const auto& file : e.libraryFiles) {
        args.emplace_back("-v");
        args.emplace_back(file);
    }
    for (const auto& file : e.files) {
        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".vlt") == 0) continue;
        args.emplace_back(file);
    }
    return args;
}

}  // namespace

std::string_view toString(TokenClass cls) { return CLASS_NAMES[static_cast<size_t>(cls)]; }
const std::vector<std::string_view>& classNames() { return CLASS_NAMES; }
const std::vector<std::string_view>& modifierNames() { return MODIFIER_NAMES; }

IndexRequest requestFromVdb(const Json& document, std::string baseDir) {
    const Json* elaboration = document.get("elaboration");
    if (!elaboration || !elaboration->isObject()) {
        throw std::runtime_error{"VDB has no elaboration record"};
    }
    IndexRequest request;
    request.baseDir = std::move(baseDir);
    request.top = std::string{document.at("top").str()};
    Elaboration& e = request.elaboration;
    e.workDir = std::string{elaboration->at("work_dir").str()};
    e.language = std::string{elaboration->at("language").str()};
    const auto strings = [&](std::string_view key, std::vector<std::string>& out) {
        for (const Json& item : elaboration->at(key).items) out.emplace_back(item.str());
    };
    strings("files", e.files);
    strings("library_files", e.libraryFiles);
    strings("include_dirs", e.includeDirs);
    strings("library_exts", e.libraryExts);
    for (const Json& define : elaboration->at("defines").items) {
        e.defines.emplace_back(define.at("name").str(), define.at("value").str());
    }
    for (const Json& source : document.at("sources").items) {
        request.sources.emplace_back(source.at("path").str());
    }
    return request;
}

SourceIndex buildIndex(const IndexRequest& request) {
    std::filesystem::path workDir{request.elaboration.workDir};
    if (workDir.is_relative()) workDir = std::filesystem::path{request.baseDir} / workDir;
    workDir = canonicalOrSelf(workDir);
    const std::filesystem::path previous = std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::current_path(workDir, ec);
    if (ec) throw std::runtime_error{"cannot enter work_dir " + workDir.string()};
    struct Restore {
        const std::filesystem::path& path;
        ~Restore() {
            std::error_code ignored;
            std::filesystem::current_path(path, ignored);
        }
    } restore{previous};

    driver::Driver driver;
    driver.addStandardArgs();
    const std::vector<std::string> args = arguments(request);
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data())
        || !driver.processOptions()) {
        throw std::runtime_error{"slang rejected the elaboration record"};
    }
    // Every syntax error is tolerated; the index covers what slang understood.
    (void)driver.parseAllSources();
    if (driver.syntaxTrees.empty()) throw std::runtime_error{"no design files could be parsed"};
    auto compilation = driver.createCompilation();
    const SourceManager& sm = driver.sourceManager;

    Semantics semantics{sm, *compilation, {}, {}};
    DesignVisitor design{semantics};
    compilation->getRoot().visit(design);
    for (const Symbol* definition : compilation->getDefinitions()) semantics.declare(*definition);
    for (const PackageSymbol* package : compilation->getPackages()) semantics.declare(*package);

    Builder builder{sm, semantics};
    for (const BufferID buffer : sm.getAllBuffers()) {
        // Only files on disk: macro expansions, command-line defines and the
        // built-in std package have no text a viewer could show.
        std::error_code ignored;
        if (sm.isFileLoc(SourceLocation{buffer, 0}) && !sm.getFullPath(buffer).empty()
            && std::filesystem::is_regular_file(sm.getFullPath(buffer), ignored)) {
            builder.registerBuffer(buffer);
        }
    }
    LexicalVisitor lexical{builder};
    for (const auto& tree : driver.syntaxTrees) tree->root().visit(lexical);

    SourceIndex index;
    index.producer = "slang " + std::to_string(VersionInfo::getMajor()) + "."
                     + std::to_string(VersionInfo::getMinor()) + "."
                     + std::to_string(VersionInfo::getPatch());
    for (const Diagnostic& diag : compilation->getAllDiagnostics()) {
        if (driver.diagEngine.getSeverity(diag.code, diag.location) >= DiagnosticSeverity::Error) {
            ++index.errors;
        }
    }

    InstanceVisitor instances;
    compilation->getRoot().visit(instances);
    for (auto& [path, blocks] : instances.found) {
        std::vector<InactiveRange> ranges;
        for (const GenerateBlockSymbol* block : blocks) {
            const auto* syntax = block->getSyntax();
            if (!syntax) continue;
            const SourceRange range = syntax->sourceRange();
            if (!range.start().valid() || sm.isMacroLoc(range.start())
                || range.start().buffer() != range.end().buffer()) {
                continue;
            }
            const int32_t file = builder.fileIndex(range.start().buffer());
            if (file < 0) continue;
            ranges.push_back(InactiveRange{
                file, Range{static_cast<uint32_t>(sm.getLineNumber(range.start())),
                            static_cast<uint32_t>(sm.getColumnNumber(range.start())),
                            static_cast<uint32_t>(sm.getLineNumber(range.end())),
                            static_cast<uint32_t>(sm.getColumnNumber(range.end()))}});
        }
        if (!ranges.empty()) index.inactive[path] = std::move(ranges);
    }
    for (const Symbol* definition : compilation->getDefinitions()) {
        if (const Location loc = builder.locate(definition->location); loc.valid()) {
            index.definitions[std::string{definition->name}] = loc;
        }
    }
    for (const PackageSymbol* package : compilation->getPackages()) {
        if (const Location loc = builder.locate(package->location); loc.valid()) {
            index.definitions[std::string{package->name}] = loc;
        }
    }

    // Name files as the VDB does, so declarations join its symbol locations.
    flat_hash_map<std::string, std::string> knownPaths;
    for (const auto& source : request.sources) {
        knownPaths.emplace(canonicalOrSelf(workDir / source).string(), source);
    }
    for (Builder::File& file : builder.files()) {
        const std::filesystem::path full = canonicalOrSelf(sm.getFullPath(file.buffer));
        std::string path;
        if (const auto it = knownPaths.find(full.string()); it != knownPaths.end()) {
            path = it->second;
        } else {
            const auto relative = full.lexically_relative(workDir);
            path = relative.empty() || relative.string().rfind("..", 0) == 0 ? full.string()
                                                                             : relative.string();
        }
        std::stable_sort(file.tokens.begin(), file.tokens.end(),
                         [](const Token& a, const Token& b) {
                             return std::tie(a.line, a.column) < std::tie(b.line, b.column);
                         });
        file.tokens.erase(std::unique(file.tokens.begin(), file.tokens.end(),
                                      [](const Token& a, const Token& b) {
                                          return a.line == b.line && a.column == b.column;
                                      }),
                          file.tokens.end());
        index.files.push_back(FileIndex{std::move(path), std::move(file.tokens)});
    }
    return index;
}

std::string toJson(const SourceIndex& index) {
    JsonWriter writer;
    writer.setPrettyPrint(false);
    writer.startObject();
    writer.writeProperty("producer");
    writer.writeValue(index.producer);
    writer.writeProperty("classes");
    writer.startArray();
    for (const auto name : CLASS_NAMES) writer.writeValue(name);
    writer.endArray();
    writer.writeProperty("modifiers");
    writer.startArray();
    for (const auto name : MODIFIER_NAMES) writer.writeValue(name);
    writer.endArray();
    writer.writeProperty("files");
    writer.startArray();
    for (const FileIndex& file : index.files) {
        writer.startObject();
        writer.writeProperty("path");
        writer.writeValue(file.path);
        writer.writeProperty("tokens");
        writer.startArray();
        for (const Token& token : file.tokens) {
            writer.writeValue(static_cast<uint64_t>(token.line));
            writer.writeValue(static_cast<uint64_t>(token.column));
            writer.writeValue(static_cast<uint64_t>(token.length));
            writer.writeValue(static_cast<uint64_t>(token.cls));
            writer.writeValue(static_cast<uint64_t>(token.modifiers));
        }
        writer.endArray();
        writer.writeProperty("declarations");
        writer.startArray();
        for (size_t i = 0; i < file.tokens.size(); ++i) {
            const Location& declaration = file.tokens[i].declaration;
            if (!declaration.valid()) continue;
            writer.writeValue(static_cast<uint64_t>(i));
            writer.writeValue(static_cast<uint64_t>(declaration.file));
            writer.writeValue(static_cast<uint64_t>(declaration.line));
            writer.writeValue(static_cast<uint64_t>(declaration.column));
        }
        writer.endArray();
        writer.endObject();
    }
    writer.endArray();
    writer.writeProperty("definitions");
    writer.startObject();
    for (const auto& [name, location] : index.definitions) {
        writer.writeProperty(name);
        writer.startArray();
        writer.writeValue(static_cast<uint64_t>(location.file));
        writer.writeValue(static_cast<uint64_t>(location.line));
        writer.writeValue(static_cast<uint64_t>(location.column));
        writer.endArray();
    }
    writer.endObject();
    writer.writeProperty("inactive");
    writer.startObject();
    for (const auto& [instance, ranges] : index.inactive) {
        writer.writeProperty(instance);
        writer.startArray();
        for (const InactiveRange& inactive : ranges) {
            writer.startArray();
            writer.writeValue(static_cast<uint64_t>(inactive.file));
            writer.writeValue(static_cast<uint64_t>(inactive.range.startLine));
            writer.writeValue(static_cast<uint64_t>(inactive.range.startColumn));
            writer.writeValue(static_cast<uint64_t>(inactive.range.endLine));
            writer.writeValue(static_cast<uint64_t>(inactive.range.endColumn));
            writer.endArray();
        }
        writer.endArray();
    }
    writer.endObject();
    writer.endObject();
    return std::string{writer.view()};
}

std::string withSourceIndex(std::string_view document, std::string_view indexJson) {
    std::vector<MemberSpan> spans;
    const Json parsed = parseJson(document, &spans);
    if (!parsed.isObject()) throw std::runtime_error{"VDB document is not a JSON object"};
    std::string text{document};
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].key != "source_index") continue;
        const size_t start = i > 0 ? spans[i - 1].end : spans[i].start;
        const size_t end
            = i > 0 ? spans[i].end : (i + 1 < spans.size() ? spans[i + 1].start : spans[i].end);
        text.erase(start, end - start);
        break;
    }
    const size_t close = text.find_last_of('}');
    if (close == std::string::npos) throw std::runtime_error{"VDB document is not terminated"};
    std::string member = spans.size() > 1 || (spans.size() == 1 && spans[0].key != "source_index")
                             ? ",\"source_index\":"
                             : "\"source_index\":";
    member += indexJson;
    text.insert(close, member);
    return text;
}

}  // namespace vdb_index
