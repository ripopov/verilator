// -*- mode: C++; c-file-style: "cc-mode" -*-
// DESCRIPTION: Verilator: Export elaborated RTL before optimization.
//
// Index module-local declarations once, then instantiate their source semantics
// along the elaborated cell hierarchy. The independent RTL VDB schema is shared
// with the slang adapter. Unsupported constructs retain connectivity and explicit
// diagnostics. No AST node is changed and no runtime waveform data is exported.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitVdb.h"

#include "V3File.h"
#include "V3Global.h"
#include "V3Os.h"
#include "V3Stats.h"
#include "V3String.h"

#include <fstream>
#include <sstream>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {
using Fields = std::vector<std::pair<string, string>>;
using Items = std::vector<string>;

string quote(const string& text) {
    string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (c < 32) {
            static constexpr char HEX[] = "0123456789abcdef";
            out += "\\u00";
            out += HEX[c >> 4];
            out += HEX[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}
string array(const Items& items) {
    string out = "[";
    for (const string& item : items) {
        if (out.size() > 1) out += ',';
        out += item;
    }
    return out + ']';
}
string object(const Fields& fields) {
    string out = "{";
    for (const auto& field : fields) {
        if (out.size() > 1) out += ',';
        out += quote(field.first) + ':' + field.second;
    }
    return out + '}';
}
string strings(const std::set<string>& values) {
    Items items;
    for (const string& value : values) items.push_back(quote(value));
    return array(items);
}
string source(const AstNode* nodep) {
    const FileLine* const flp = nodep->fileline();
    return object({{"file", quote(flp->filename())},
                   {"line", cvtToStr(flp->firstLineno())},
                   {"column", cvtToStr(flp->firstColumn())}});
}
string type(const AstNodeDType* dtypep) {
    dtypep = dtypep->skipRefp();
    return object({{"text", quote(dtypep->prettyDTypeName(true))},
                   {"width", cvtToStr(dtypep->width())},
                   {"signed", dtypep->isSigned() ? "true" : "false"},
                   {"four_state", dtypep->isFourstate() ? "true" : "false"},
                   {"integral", dtypep->isIntegralOrPacked() ? "true" : "false"}});
}
string direction(const AstVar* varp) {
    return varp->isInout() ? "InOut" : varp->isRef() ? "Ref" : !varp->isNonOutput() ? "Out" : "In";
}

struct CellInstance final {
    const AstCell* cellp;
    string name;
    int element;
    int count;
};
struct ModuleIndex final {
    std::map<const AstVar*, string> names;
    std::vector<const AstVar*> vars;
    std::map<string, const AstEnumItem*> enums;
    std::vector<CellInstance> cells;
    std::vector<const AstNode*> processes;
};

class IndexVisitor final : public VNVisitorConst {
    ModuleIndex& m_index;
    string m_scope;
    bool m_inProcess = false;
    void visit(AstBegin* nodep) override {
        VL_RESTORER(m_scope);
        if (!nodep->name().empty()) m_scope += nodep->prettyName() + '.';
        iterateChildrenConst(nodep);
    }
    void visit(AstGenBlock* nodep) override {
        VL_RESTORER(m_scope);
        if (!nodep->name().empty()) m_scope += nodep->prettyName() + '.';
        iterateChildrenConst(nodep);
    }
    void visit(AstVar* nodep) override {
        if (nodep->isGenVar()) return;
        m_index.names.emplace(nodep, m_scope + nodep->prettyName());
        m_index.vars.push_back(nodep);
    }
    void visit(AstTypedef* nodep) override {
        if (const AstEnumDType* const ep
            = VN_CAST(nodep->subDTypep()->skipRefToEnump(), EnumDType)) {
            for (const AstEnumItem* ip = ep->itemsp(); ip; ip = VN_CAST(ip->nextp(), EnumItem)) {
                m_index.enums.emplace(m_scope + ip->prettyName(), ip);
            }
        }
    }
    void cellElements(const AstCell* cellp, const AstRange* rangep, const string& name,
                      int element, int count) {
        if (!rangep) {
            m_index.cells.push_back({cellp, name, element, count});
            return;
        }
        const int size = rangep->elementsConst();
        for (int i = 0; i < size; ++i) {
            const int offset = rangep->ascending() ? size - 1 - i : i;
            cellElements(cellp, VN_CAST(rangep->nextp(), Range),
                         name + '[' + cvtToStr(rangep->loConst() + i) + ']',
                         element * size + offset, count);
        }
    }
    void visit(AstCell* nodep) override {
        int count = 1;
        for (const AstRange* rp = nodep->rangep(); rp; rp = VN_CAST(rp->nextp(), Range)) {
            count *= rp->elementsConst();
        }
        cellElements(nodep, nodep->rangep(), m_scope + nodep->prettyName(), 0, count);
    }
    void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_inProcess);
        m_inProcess = true;
        m_index.processes.push_back(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstAssignW* nodep) override {
        if (!m_inProcess) m_index.processes.push_back(nodep);
    }
    void visit(AstNodeFTask*) override {}
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    IndexVisitor(const AstNodeModule* modp, ModuleIndex& index)
        : m_index{index} {
        iterateChildrenConst(const_cast<AstNodeModule*>(modp));
    }
};

class VdbEmitter final {
    std::map<const AstNodeModule*, ModuleIndex> m_modules;
    std::map<string, const AstVar*> m_symbolPaths;
    const ModuleIndex* m_indexp = nullptr;
    string m_path;
    Fields m_symbols;
    Items m_instances;
    Items m_connections;
    Items m_processes;

    const ModuleIndex& index(const AstNodeModule* modp) {
        const auto inserted = m_modules.emplace(modp, ModuleIndex{});
        if (inserted.second) IndexVisitor{modp, inserted.first->second};
        return inserted.first->second;
    }
    void indexPaths(const AstNodeModule* modp, const string& path) {
        const ModuleIndex& local = index(modp);
        for (const auto& var : local.names)
            m_symbolPaths.emplace(path + '.' + var.second, var.first);
        for (const auto& cell : local.cells)
            indexPaths(cell.cellp->modp(), path + '.' + cell.name);
    }
    string symbol(const AstNodeVarRef* refp) const {
        if (const AstVarXRef* const xp = VN_CAST(refp, VarXRef)) {
            const string dotted = AstNode::prettyName(xp->dotted());
            const string leaf = (dotted.empty() ? "" : dotted + '.')
                                + AstNode::prettyName(refp->varp()->origName());
            string scope = m_path;
            while (true) {
                const string candidate = (scope.empty() ? "" : scope + '.') + leaf;
                const auto it = m_symbolPaths.find(candidate);
                if (it != m_symbolPaths.end() && it->second == refp->varp()) return candidate;
                if (scope.empty()) break;
                const size_t dot = scope.rfind('.');
                scope = dot == string::npos ? "" : scope.substr(0, dot);
            }
            return m_path + '.' + leaf;
        }
        const auto it = m_indexp->names.find(refp->varp());
        return m_path + '.'
               + (it == m_indexp->names.end() ? AstNode::prettyName(refp->varp()->origName())
                                              : it->second);
    }
    class References final : public VNVisitorConst {
        const VdbEmitter& m_emitter;
        void visit(AstNodeVarRef* nodep) override {
            if (nodep->varp()->isParam()) return;
            const string path = m_emitter.symbol(nodep);
            if (nodep->access().isReadOrRW()) reads.insert(path);
            if (nodep->access().isWriteOrRW()) targets.insert(path);
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        std::set<string> reads;
        std::set<string> targets;
        References(const VdbEmitter& emitter, const AstNode* nodep)
            : m_emitter{emitter} {
            if (nodep) iterateConst(const_cast<AstNode*>(nodep));
        }
    };
    string expression(const AstNodeExpr* nodep) {
        Fields fields{{"type", type(nodep->dtypep())}, {"source", source(nodep)}};
        const auto finish = [&fields](const string& kind, const Fields& more) {
            fields.emplace_back("kind", quote(kind));
            fields.insert(fields.end(), more.begin(), more.end());
            return object(fields);
        };
        if (const AstConst* const cp = VN_CAST(nodep, Const)) {
            if (!cp->num().isOpaque())
                return finish("Constant", {{"value", quote(cp->num().ascii(true, true))}});
        } else if (const AstEnumItemRef* const ep = VN_CAST(nodep, EnumItemRef)) {
            return expression(ep->itemp()->valuep());
        } else if (const AstNodeVarRef* const rp = VN_CAST(nodep, NodeVarRef)) {
            if (rp->varp()->isParam() && VN_IS(rp->varp()->valuep(), Const)) {
                return expression(VN_AS(rp->varp()->valuep(), Const));
            }
            return finish("NamedValue", {{"symbol", quote(symbol(rp))}});
        } else if (const AstCond* const cp = VN_CAST(nodep, Cond)) {
            return finish("ConditionalOp", {{"cond", expression(cp->condp())},
                                            {"yes", expression(cp->thenp())},
                                            {"no", expression(cp->elsep())}});
        } else if (const AstSel* const sp = VN_CAST(nodep, Sel)) {
            // Width-generated truncations are conversions, not source part selects.
            if (!sp->declRange().ranged() && VN_IS(sp->lsbp(), Const) && sp->lsbConst() == 0) {
                return finish("Conversion", {{"operand", expression(sp->fromp())}});
            }
            // Width resolution normalizes indices to zero-based bit offsets.
            Fields select{{"value", expression(sp->fromp())},
                          {"range_left", cvtToStr(sp->fromp()->width() - 1)},
                          {"range_right", "0"}};
            if (sp->widthConst() == 1) {
                select.emplace_back("selector", expression(sp->lsbp()));
                return finish("ElementSelect", select);
            }
            select.emplace_back("left", expression(sp->lsbp()));
            select.emplace_back("right", object({{"kind", quote("Constant")},
                                                 {"value", quote(cvtToStr(sp->widthConst()))},
                                                 {"type", type(sp->lsbp()->dtypep())},
                                                 {"source", source(sp)}}));
            select.emplace_back("selection", quote("IndexedUp"));
            return finish("RangeSelect", select);
        } else if (const AstReplicate* const rp = VN_CAST(nodep, Replicate)) {
            return finish("Replication",
                          {{"count", cvtToStr(VN_AS(rp->countp(), Const)->toUInt())},
                           {"operand", expression(rp->srcp())}});
        } else if (const AstConcat* const cp = VN_CAST(nodep, Concat)) {
            return finish("Concatenation",
                          {{"operands", array({expression(cp->lhsp()), expression(cp->rhsp())})}});
        } else if (const AstNodeBiop* const bp = VN_CAST(nodep, NodeBiop)) {
            static const std::map<string, string> ops{{"ADD", "Add"},
                                                      {"SUB", "Subtract"},
                                                      {"MUL", "Multiply"},
                                                      {"MULS", "Multiply"},
                                                      {"DIV", "Divide"},
                                                      {"DIVS", "Divide"},
                                                      {"MODDIV", "Mod"},
                                                      {"MODDIVS", "Mod"},
                                                      {"AND", "BinaryAnd"},
                                                      {"OR", "BinaryOr"},
                                                      {"XOR", "BinaryXor"},
                                                      {"LOGAND", "LogicalAnd"},
                                                      {"LOGOR", "LogicalOr"},
                                                      {"EQ", "Equality"},
                                                      {"NEQ", "Inequality"},
                                                      {"EQCASE", "CaseEquality"},
                                                      {"NEQCASE", "CaseInequality"},
                                                      {"LT", "LessThan"},
                                                      {"LTS", "LessThan"},
                                                      {"LTE", "LessThanEqual"},
                                                      {"LTES", "LessThanEqual"},
                                                      {"GT", "GreaterThan"},
                                                      {"GTS", "GreaterThan"},
                                                      {"GTE", "GreaterThanEqual"},
                                                      {"GTES", "GreaterThanEqual"},
                                                      {"SHIFTL", "LogicalShiftLeft"},
                                                      {"SHIFTR", "LogicalShiftRight"},
                                                      {"SHIFTRS", "ArithmeticShiftRight"}};
            const auto it = ops.find(nodep->typeName());
            if (it != ops.end())
                return finish("BinaryOp", {{"op", quote(it->second)},
                                           {"left", expression(bp->lhsp())},
                                           {"right", expression(bp->rhsp())}});
        } else if (const AstNodeUniop* const up = VN_CAST(nodep, NodeUniop)) {
            const string name = nodep->typeName();
            if (name == "EXTEND" || name == "EXTENDS" || name == "SIGNED" || name == "UNSIGNED"
                || name == "CAST" || name == "CCAST") {
                return finish("Conversion", {{"operand", expression(up->lhsp())}});
            }
            static const std::map<string, string> ops{
                {"NEGATE", "Minus"},      {"NOT", "BitwiseNot"},  {"LOGNOT", "LogicalNot"},
                {"REDAND", "BitwiseAnd"}, {"REDOR", "BitwiseOr"}, {"REDXOR", "BitwiseXor"}};
            const auto it = ops.find(name);
            if (it != ops.end())
                return finish("UnaryOp",
                              {{"op", quote(it->second)}, {"operand", expression(up->lhsp())}});
        }
        return finish("Unsupported",
                      {{"reason", quote("Verilator expression " + string{nodep->typeName()})},
                       {"reads", strings(References{*this, nodep}.reads)}});
    }
    string unsupported(const AstNode* nodep, const string& reason) const {
        return object({{"kind", quote("Unsupported")},
                       {"reason", quote(reason)},
                       {"source", source(nodep)}});
    }
    string statement(const AstNode* nodep) {
        if (const AstNodeAssign* const ap = VN_CAST(nodep, NodeAssign)) {
            const AstNodeVarRef* const lhsp = VN_CAST(ap->lhsp(), NodeVarRef);
            if (!lhsp || ap->timingControlp())
                return unsupported(nodep, "partial/aggregate or timed assignment");
            return object({{"kind", quote("Assign")},
                           {"target", quote(symbol(lhsp))},
                           {"value", expression(ap->rhsp())},
                           {"nba", VN_IS(ap, AssignDly) ? "true" : "false"},
                           {"source", source(ap)}});
        }
        if (const AstIf* const ip = VN_CAST(nodep, If)) {
            return object({{"kind", quote("If")},
                           {"cond", expression(ip->condp())},
                           {"yes", statements(ip->thensp())},
                           {"no", statements(ip->elsesp())},
                           {"source", source(ip)}});
        }
        if (const AstBegin* const bp = VN_CAST(nodep, Begin)) return statements(bp->stmtsp());
        if (VN_IS(nodep, Var) || VN_IS(nodep, Comment)) return object({{"kind", quote("Empty")}});
        return unsupported(nodep, "Verilator statement " + string{nodep->typeName()});
    }
    string statements(const AstNode* nodep) {
        Items stmts;
        for (; nodep; nodep = nodep->nextp()) stmts.push_back(statement(nodep));
        return object({{"kind", quote("Sequence")}, {"statements", array(stmts)}});
    }
    void addProcess(const AstNode* nodep, const string& origin, const string& mode,
                    const std::set<string>& reads, const std::set<string>& targets,
                    const Items& events, const string& body, const string& owner) {
        m_processes.push_back(object({{"id", cvtToStr(m_processes.size())},
                                      {"owner", quote(owner)},
                                      {"origin", quote(origin)},
                                      {"mode", quote(mode)},
                                      {"reads", strings(reads)},
                                      {"targets", strings(targets)},
                                      {"events", array(events)},
                                      {"body", body},
                                      {"source", source(nodep)}}));
    }
    void process(const AstNode* nodep) {
        string mode = "unsupported";
        string body;
        Items events;
        if (const AstAlways* const ap = VN_CAST(nodep, Always)) {
            mode = (ap->keyword() == VAlwaysKwd::ALWAYS_COMB
                    || ap->keyword() == VAlwaysKwd::CONT_ASSIGN)
                       ? "comb"
                       : "unsupported";
            if (ap->sentreep()) {
                mode = "seq";
                for (const AstSenItem* sp = ap->sentreep()->sensesp(); sp;
                     sp = VN_CAST(sp->nextp(), SenItem)) {
                    if (sp->isComboOrStar()) {
                        mode = "comb";
                        continue;
                    }
                    if (sp->condp()
                        || (sp->edgeType() != VEdgeType::ET_POSEDGE
                            && sp->edgeType() != VEdgeType::ET_NEGEDGE)) {
                        mode = "unsupported";
                        break;
                    }
                    events.push_back(object(
                        {{"edge",
                          quote(sp->edgeType() == VEdgeType::ET_POSEDGE ? "PosEdge" : "NegEdge")},
                         {"expr", expression(sp->sensp())},
                         {"source", source(sp)}}));
                }
            }
            body = ap->keyword() == VAlwaysKwd::CONT_ASSIGN && ap->isJustOneBodyStmt()
                       ? statement(ap->stmtsp())
                       : statements(ap->stmtsp());
        } else if (VN_IS(nodep, AssignW)) {
            mode = "comb";
            body = statement(nodep);
        } else {
            body = unsupported(nodep, "Verilator process " + string{nodep->typeName()});
        }
        const References refs{*this, nodep};
        addProcess(nodep, "rtl", mode, refs.reads, refs.targets, events, body, m_path);
    }
    void instance(const AstNodeModule* modp, const string& path, const string& parent,
                  const AstCell* cellp) {
        VL_RESTORER(m_indexp);
        VL_RESTORER(m_path);
        m_indexp = &index(modp);
        m_path = path;
        Items ports;
        for (const AstVar* const vp : m_indexp->vars) {
            // Interface references name an instance, not a value.
            if (vp->isIfaceRef()) continue;
            const string name = path + '.' + m_indexp->names.at(vp);
            Fields fields{{"path", quote(name)},
                          {"owner", quote(path)},
                          {"kind", quote(vp->isParam() ? "Parameter"
                                         : vp->isNet() ? "Net"
                                                       : "Variable")},
                          {"type", type(vp->dtypep())},
                          {"source", source(vp)}};
            if (const AstConst* const cp = VN_CAST(vp->valuep(), Const))
                fields.emplace_back("value", quote(cp->num().ascii(true, true)));
            m_symbols.emplace_back(name, object(fields));
            if (vp->isIO())
                ports.push_back(object({{"name", quote(AstNode::prettyName(vp->origName()))},
                                        {"symbol", quote(name)},
                                        {"direction", quote(direction(vp))}}));
        }
        for (const auto& item : m_indexp->enums) {
            const AstEnumItem* const ep = item.second;
            const string name = path + '.' + item.first;
            m_symbols.emplace_back(
                name,
                object({{"path", quote(name)},
                        {"owner", quote(path)},
                        {"kind", quote("EnumValue")},
                        {"type", type(ep->dtypep())},
                        {"source", source(ep)},
                        {"value", quote(VN_AS(ep->valuep(), Const)->num().ascii(true, true))}}));
        }
        m_instances.push_back(
            object({{"path", quote(path)},
                    {"parent", parent.empty() ? "null" : quote(parent)},
                    {"definition", quote(modp->origName())},
                    {"ports", array(ports)},
                    {"source", source(cellp ? static_cast<const AstNode*>(cellp) : modp)}}));
        for (const AstNode* const pp : m_indexp->processes) process(pp);
        for (const auto& child : m_indexp->cells) {
            const AstCell* const cp = child.cellp;
            const string childPath = path + '.' + child.name;
            const ModuleIndex& childIndex = index(cp->modp());
            for (const AstPin* pinp = cp->pinsp(); pinp; pinp = VN_CAST(pinp->nextp(), Pin)) {
                if (!pinp->modVarp() || !pinp->exprp()) continue;
                const AstVar* const vp = pinp->modVarp();
                // Interface references are not data ports; the connected interface
                // instance is exported as a scope instead.
                if (!vp->isIO()) continue;
                const string port = childPath + '.' + childIndex.names.at(vp);
                const AstNodeExpr* const ep = VN_CAST(pinp->exprp(), NodeExpr);
                if (!ep) continue;
                string expr = expression(ep);
                const bool distributed = child.count > 1 && ep->width() != vp->width();
                if (distributed) {
                    const string integerType = object({{"text", quote("int")},
                                                       {"width", "32"},
                                                       {"signed", "false"},
                                                       {"four_state", "false"},
                                                       {"integral", "true"}});
                    const auto constant = [&](int value) {
                        return object({{"kind", quote("Constant")},
                                       {"value", quote(cvtToStr(value))},
                                       {"type", integerType},
                                       {"source", source(pinp)}});
                    };
                    expr = object({{"kind", quote("RangeSelect")},
                                   {"type", type(vp->dtypep())},
                                   {"source", source(pinp)},
                                   {"value", expr},
                                   {"left", constant(child.element * vp->width())},
                                   {"right", constant(vp->width())},
                                   {"selection", quote("IndexedUp")},
                                   {"range_left", cvtToStr(ep->width() - 1)},
                                   {"range_right", "0"}});
                }
                const string dir = direction(vp);
                m_connections.push_back(object({{"instance", quote(childPath)},
                                                {"port", quote(port)},
                                                {"direction", quote(dir)},
                                                {"expression", expr},
                                                {"source", source(pinp)}}));
                string target = port;
                string value = expr;
                if (!vp->isNonOutput()) {
                    const AstNodeVarRef* const rp = VN_CAST(ep, NodeVarRef);
                    target = rp && !distributed ? symbol(rp) : "";
                    value = object({{"kind", quote("NamedValue")},
                                    {"symbol", quote(port)},
                                    {"type", type(vp->dtypep())},
                                    {"source", source(pinp)}});
                }
                const bool supported = (dir == "In" || dir == "Out") && !target.empty();
                const string body
                    = supported ? object({{"kind", quote("Assign")},
                                          {"target", quote(target)},
                                          {"value", value},
                                          {"nba", "false"},
                                          {"source", source(pinp)}})
                                : unsupported(pinp, "inout/ref or complex output connection");
                std::set<string> targets
                    = supported ? std::set<string>{target} : References{*this, ep}.targets;
                if (targets.empty()) targets.insert(port);
                addProcess(pinp, "connection", "comb", {}, targets, {}, body, path);
            }
            instance(cp->modp(), childPath, path, cp);
        }
    }

public:
    void emit() {
        const AstNodeModule* const topp = v3Global.rootp()->topModulep();
        indexPaths(topp, topp->origName());
        instance(topp, topp->origName(), "", nullptr);
        Items sources;
        for (const string& file : FileLine::filenames()) {
            std::ifstream stream{file, std::ios::binary};
            if (!stream) continue;
            const string bytes{std::istreambuf_iterator<char>{stream},
                               std::istreambuf_iterator<char>{}};
            sources.push_back(object(
                {{"path", quote(file)}, {"sha256", quote(VHashSha256{bytes}.digestHex())}}));
        }
        // Structured elaboration inputs so design browsers can re-elaborate the same
        // source set with another frontend. Paths are as given on the command line and
        // resolve relative to work_dir.
        Items files;
        for (const VFileLibName& file : v3Global.opt.vFiles()) files.push_back(quote(file.filename()));
        Items libraryFiles;
        for (const VFileLibName& file : v3Global.opt.libraryFiles()) {
            libraryFiles.push_back(quote(file.filename()));
        }
        Items includeDirs;
        for (const string& dir : v3Global.opt.incDirUsers()) includeDirs.push_back(quote(dir));
        Items libraryExts;
        for (const string& ext : v3Global.opt.libExtVs()) libraryExts.push_back(quote(ext));
        Items defines;
        for (const auto& define : v3Global.opt.cmdDefines()) {
            defines.push_back(
                object({{"name", quote(define.first)}, {"value", quote(define.second)}}));
        }
        const string elaboration
            = object({{"work_dir", quote(V3Os::filenameRealPath("."))},
                      {"language", quote(v3Global.opt.defaultLanguage().ascii())},
                      {"files", array(files)},
                      {"library_files", array(libraryFiles)},
                      {"include_dirs", array(includeDirs)},
                      {"library_exts", array(libraryExts)},
                      {"defines", array(defines)}});
        Fields fields{{"format", quote("vtr-rtl-vdb")},
                      {"version", "2"},
                      {"producer", quote(V3Options::version())},
                      {"top", quote(topp->origName())},
                      {"options", array({quote(v3Global.opt.allArgsString())})},
                      {"elaboration", elaboration},
                      {"sources", array(sources)},
                      {"instances", array(m_instances)},
                      {"symbols", object(m_symbols)},
                      {"connections", array(m_connections)},
                      {"processes", array(m_processes)}};
        const string id = VHashSha256{object(fields)}.digestHex();
        fields.emplace_back("design_id", quote(id));
        const string doc = object(fields);
        v3Global.vdbDocument(doc);
        v3Global.vdbId(id);
        const string filename = v3Global.opt.makeDir() + '/' + v3Global.opt.prefix() + ".vdb.json";
        const std::unique_ptr<std::ofstream> outp{V3File::new_ofstream(filename)};
        *outp << doc << '\n';
        V3Stats::addStat("VDB, symbols", m_symbols.size());
        V3Stats::addStat("VDB, instances", m_instances.size());
        V3Stats::addStat("VDB, processes", m_processes.size());
    }
};
}  // namespace

void V3EmitVdb::emit() { VdbEmitter{}.emit(); }
