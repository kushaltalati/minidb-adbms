#include "minidb/query/optimizer.h"

#include <set>
#include <sstream>

#include "minidb/exceptions.h"

namespace minidb {

namespace {

struct Relation {
    std::string alias;       // alias or, if none, the table name
    TableHandle* table;
};

std::string alias_of(const std::string& table, const std::string& alias) {
    return alias.empty() ? table : alias;
}

// Cost model constant. A sequential scan reads N rows with cheap sequential
// I/O; an index scan fetches ~selectivity*N rows but each match is a *random*
// access (descend the tree, then chase a RID to its heap page). We weight each
// indexed match by this penalty so the optimizer can decide an index is *not*
// worth it when the predicate isn't selective enough. With this value a unique
// point lookup and a non-unique equality favour the index, while a broad range
// (sel ~= 1/3) or a tiny table favour a full scan -- a real cost-based choice.
constexpr double kRandomAccessPenalty = 4.0;

// Which relation does a column reference belong to? Returns index into `rels`.
int resolve_relation(const std::vector<Relation>& rels, const ColumnRef& ref) {
    if (!ref.table.empty()) {
        for (std::size_t i = 0; i < rels.size(); ++i)
            if (rels[i].alias == ref.table) return static_cast<int>(i);
        throw SQLException("unknown table/alias '" + ref.table + "'");
    }
    int found = -1;
    for (std::size_t i = 0; i < rels.size(); ++i) {
        if (rels[i].table->schema->column_index(ref.col) >= 0) {
            if (found != -1)
                throw SQLException("ambiguous column '" + ref.col + "'");
            found = static_cast<int>(i);
        }
    }
    if (found == -1) throw SQLException("unknown column '" + ref.col + "'");
    return found;
}

// Relations referenced by a predicate (1 = single-table filter, 2 = join).
std::set<int> relations_of(const std::vector<Relation>& rels,
                           const Predicate& p) {
    std::set<int> s;
    s.insert(resolve_relation(rels, p.left));
    if (p.right_is_column) s.insert(resolve_relation(rels, p.right_col));
    return s;
}

// Build a scan for one relation, pushing down its single-table filters and
// choosing an index scan when a usable indexed predicate exists.
std::unique_ptr<Operator> build_scan(ExecContext* ctx, const Relation& rel,
                                     const std::vector<Predicate>& filters) {
    const Schema& schema = *rel.table->schema;

    // Look for an indexable predicate: column (of this relation) compared with
    // a literal, on a column that has an index, with EQ or a range operator.
    int chosen = -1;     // index into filters
    bool chosen_is_eq = false;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        const Predicate& p = filters[i];
        if (p.right_is_column) continue;          // not a column-vs-literal test
        if (p.op == CompOp::NE) continue;          // index can't help "!="
        int col = schema.column_index(p.left.col);
        if (col < 0) continue;
        if (rel.table->index_on(col) == nullptr) continue;
        bool is_eq = (p.op == CompOp::EQ);
        if (is_eq) { chosen = static_cast<int>(i); chosen_is_eq = true; break; }
        if (chosen == -1) chosen = static_cast<int>(i);  // remember a range
    }

    if (chosen >= 0) {
        const Predicate& p = filters[chosen];
        int col = schema.column_index(p.left.col);
        const IndexHandle* idx = rel.table->index_on(col);

        // Cost the two access paths and only use the index when it wins.
        double n = static_cast<double>(rel.table->row_count());
        double sel = Optimizer::estimate_selectivity(p, rel.table);
        double index_cost = sel * n * kRandomAccessPenalty;
        double seq_cost = n;
        std::size_t est_rows = static_cast<std::size_t>(sel * n + 0.5);
        if (est_rows == 0) est_rows = 1;  // never advertise 0 matching rows

        if (index_cost < seq_cost) {
            std::optional<Value> lo, hi;
            bool lo_inc = true, hi_inc = true;
            switch (p.op) {
                case CompOp::EQ: lo = p.right_value; hi = p.right_value; break;
                case CompOp::GT: lo = p.right_value; lo_inc = false; break;
                case CompOp::GE: lo = p.right_value; lo_inc = true; break;
                case CompOp::LT: hi = p.right_value; hi_inc = false; break;
                case CompOp::LE: hi = p.right_value; hi_inc = true; break;
                default: break;
            }

            std::ostringstream reason;
            reason << (chosen_is_eq ? "point lookup" : "range scan") << " on "
                   << rel.table->name << "." << p.left.col
                   << (idx->unique ? " (unique)" : "")
                   << "  est_rows=" << est_rows
                   << "  [index_cost=" << index_cost
                   << " < scan_cost=" << seq_cost << "]";

            std::vector<Predicate> residual;
            for (std::size_t i = 0; i < filters.size(); ++i)
                if (static_cast<int>(i) != chosen) residual.push_back(filters[i]);

            std::unique_ptr<Operator> scan = std::make_unique<IndexScan>(
                ctx, rel.table, rel.alias, idx, lo, lo_inc, hi, hi_inc,
                reason.str());
            if (residual.empty()) return scan;
            return std::make_unique<Filter>(std::move(scan), residual);
        }
        // Index exists but isn't selective enough -> a full scan is cheaper.
        // Fall through to the SeqScan path below.
    }

    // No usable index: full table scan, with any filters applied on top.
    std::unique_ptr<Operator> scan =
        std::make_unique<SeqScan>(ctx, rel.table, rel.alias);
    if (filters.empty()) return scan;
    return std::make_unique<Filter>(std::move(scan), filters);
}

}  // namespace

double Optimizer::estimate_selectivity(const Predicate& p,
                                       const TableHandle* table) {
    if (p.right_is_column) return 0.1;  // join-style; not used for sizing here
    int col = table->schema->column_index(p.left.col);
    const IndexHandle* idx = (col >= 0) ? table->index_on(col) : nullptr;
    std::size_t n = table->row_count();
    switch (p.op) {
        case CompOp::EQ:
            if (idx && idx->unique) return n > 0 ? 1.0 / n : 1.0;
            return 0.1;                       // ~10% for a non-unique equality
        case CompOp::NE: return 0.9;
        case CompOp::LT:
        case CompOp::LE:
        case CompOp::GT:
        case CompOp::GE: return 0.33;         // a range matches ~a third
    }
    return 0.5;
}

std::unique_ptr<Operator> Optimizer::build_select(ExecContext* ctx,
                                                  const SelectStmt& stmt) {
    // 1. Resolve all relations (FROM + JOINs).
    std::vector<Relation> rels;
    rels.push_back({alias_of(stmt.from_table, stmt.from_alias),
                    ctx->tables->get_table(stmt.from_table)});
    if (!rels.back().table)
        throw CatalogException("no such table '" + stmt.from_table + "'");
    for (const auto& j : stmt.joins) {
        TableHandle* h = ctx->tables->get_table(j.table);
        if (!h) throw CatalogException("no such table '" + j.table + "'");
        rels.push_back({alias_of(j.table, j.alias), h});
    }

    // 2. Classify WHERE predicates: single-relation filters get pushed down to
    //    that relation's scan; multi-relation ones become a post-join filter.
    std::vector<std::vector<Predicate>> rel_filters(rels.size());
    std::vector<Predicate> post_join;
    for (const auto& p : stmt.where) {
        std::set<int> r = relations_of(rels, p);
        if (r.size() == 1)
            rel_filters[*r.begin()].push_back(p);
        else
            post_join.push_back(p);
    }

    // 3. Single relation: just its scan.
    if (rels.size() == 1) {
        auto plan = build_scan(ctx, rels[0], rel_filters[0]);
        for (auto& p : post_join)  // (cannot happen for 1 relation, but safe)
            plan = std::make_unique<Filter>(std::move(plan),
                                            std::vector<Predicate>{p});
        if (!stmt.select_star)
            plan = std::make_unique<Project>(std::move(plan), stmt.columns);
        return plan;
    }

    // 4. Join ordering: start from the smallest relation, then greedily attach
    //    a relation connected by a join predicate (smallest first).
    std::vector<Predicate> join_preds;
    for (const auto& j : stmt.joins) join_preds.push_back(j.on);

    int start = 0;
    for (std::size_t i = 1; i < rels.size(); ++i)
        if (rels[i].table->row_count() < rels[start].table->row_count())
            start = static_cast<int>(i);

    std::set<int> joined{start};
    std::unique_ptr<Operator> plan = build_scan(ctx, rels[start], rel_filters[start]);

    while (joined.size() < rels.size()) {
        // Find a join predicate linking a joined relation to an unjoined one.
        int next_rel = -1;
        const Predicate* link = nullptr;
        for (const auto& jp : join_preds) {
            std::set<int> r = relations_of(rels, jp);
            if (r.size() != 2) continue;
            int a = *r.begin(), b = *(++r.begin());
            bool ja = joined.count(a), jb = joined.count(b);
            if (ja && !jb) { next_rel = b; link = &jp; break; }
            if (jb && !ja) { next_rel = a; link = &jp; break; }
        }
        if (next_rel == -1)
            throw SQLException("unsupported join (relations not connected)");

        const Relation& R = rels[next_rel];
        // Which of R's columns does the join use?
        ColumnRef r_side =
            (resolve_relation(rels, link->left) == next_rel) ? link->left
                                                             : link->right_col;
        int r_col = R.table->schema->column_index(r_side.col);
        const IndexHandle* r_idx = R.table->index_on(r_col);

        bool inlj_ok = (link->op == CompOp::EQ) && r_idx != nullptr &&
                       rel_filters[next_rel].empty();
        if (inlj_ok) {
            plan = std::make_unique<IndexNestedLoopJoin>(
                ctx, std::move(plan), R.table, R.alias, r_idx, *link);
        } else {
            auto inner = build_scan(ctx, R, rel_filters[next_rel]);
            plan = std::make_unique<NestedLoopJoin>(std::move(plan),
                                                    std::move(inner), *link);
        }
        joined.insert(next_rel);
    }

    if (!post_join.empty())
        plan = std::make_unique<Filter>(std::move(plan), post_join);
    if (!stmt.select_star)
        plan = std::make_unique<Project>(std::move(plan), stmt.columns);
    return plan;
}

}  // namespace minidb
