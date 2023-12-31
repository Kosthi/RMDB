/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"
#include <regex>

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        /** TODO: 检查表是否存在 */
        for (auto& table_name : query->tables) {
            if (!sm_manager_->db_.is_table(table_name)) {
                throw TableNotFoundError(table_name);
            }
        }
        // 判断是聚合还是普通sel
        bool sel_or_agg = x->agg_clauses.empty();
        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &sv_sel_col : x->cols) {
            TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
            query->cols.push_back(sel_col);
        }

        // 是否存在count(*)
        bool is_exist = false;
        // 存储投影列和别名
        for (auto &agg_clause : x->agg_clauses) {
            // count(*)
            if (agg_clause->col->col_name.empty() && agg_clause->type == T_COUNT) {
                is_exist = true;
            }
            TabCol agg_col = {.tab_name = agg_clause->col->tab_name, .col_name = agg_clause->col->col_name};
            query->cols.emplace_back(agg_col);
            // 别名命名规则？
            if (agg_clause->nick_name.empty()) {
                std::string name;
                auto &type = agg_clause->type;
                if (type == T_SUM) name += "SUM";
                else if (type == T_MAX) name += "MAX";
                else if (type == T_MIN) name += "MIN";
                else if (type == T_COUNT) name += "COUNT";
                if (agg_col.tab_name.empty() && agg_col.col_name.empty()) {
                    name += "(*)";
                }
                else if (agg_col.tab_name.empty()) {
                    name += "(" + agg_col.col_name + ")";
                } else {
                    name += "(" + agg_col.tab_name + "." + agg_col.col_name + ")";
                }
                query->nick_names.emplace_back(name);
                continue;
            }
            query->nick_names.emplace_back(agg_clause->nick_name);
        }
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (is_exist) {
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->all_cols.push_back(sel_col);
            }
        }
        if (sel_or_agg && query->cols.empty()) {
            // select all columns
            // select *
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            // infer table name from column name
            // select t.id, or id
            for (auto &sel_col : query->cols) {
                check_column(all_cols, sel_col);  // 列元数据校验
            }
        }
        // 检查类型 聚合
        for (size_t i = 0; i < x->agg_clauses.size(); ++i) {
            switch (x->agg_clauses[i]->type) {
                case T_SUM: {
                    for (auto &col : all_cols) {
                        if (col.tab_name == query->cols[i].tab_name && col.name == query->cols[i].col_name) {
                            if (col.type == TYPE_INT || col.type == TYPE_FLOAT) {
                                break;
                            } else {
                                throw InternalError("Aggregation Type Error.");
                            }
                        }
                    }
                    break;
                }
                case T_MAX:
                case T_MIN:
                case T_COUNT: {
                    for (auto &col : all_cols) {
                        if (col.tab_name == query->cols[i].tab_name && col.name == query->cols[i].col_name) {
                            if (col.type == TYPE_INT || col.type == TYPE_FLOAT || col.type == TYPE_STRING) {
                                break;
                            } else {
                                throw InternalError("Aggregation Type Error.");
                            }
                        }
                    }
                    break;
                }
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
        // 处理order_Clause
        for (auto& order : x->orders) {
            TabCol order_col = {.tab_name = order->col->tab_name, .col_name = order->col->col_name};
            check_column(all_cols, order_col);
            order->col->tab_name = order_col.tab_name;
            order->col->col_name = order_col.col_name;
        }
        // 限制记录条数
        query->limit = x->limit;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
        // 处理 set 语句
        for (auto& clause : x->set_clauses) {
            SetClause setClause;
            setClause.lhs = {x->tab_name, clause->col_name};
            auto val = convert_sv_value(clause->val);
            if (val.type == TYPE_INT) {
                val.init_raw(sizeof(int));
            }
            else if (val.type == TYPE_FLOAT) {
                val.init_raw(sizeof(double));
            }
            else if (val.type == TYPE_BIGINT) {
                val.init_raw(sizeof(long long));
            }
            else if (val.type == TYPE_DATETIME) {
                val.init_raw(sizeof(DateTime));
            }
            else {
                val.init_raw(val.str_val.size());
            }
            setClause.rhs = val;
            query->set_clauses.emplace_back(setClause);
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


void Analyze::check_column(const std::vector<ColMeta>& all_cols, TabCol& target) {
    // only for count(*)
    // 因为两个都为空，则在语法分析时就会报错，因此这里保证必为 count(*)
    if (target.tab_name.empty() && target.col_name.empty()) return;
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        // 找到段名相同的表名，若找到多次，则返回模糊段错误
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        /** TODO: Make sure target column exists */
        bool is_find = true;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                is_find = false;
                break;
            }
        }
        if (is_find) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            // 处理整型与浮点数的类型转换
            if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.set_float(static_cast<double>(cond.rhs_val.int_val));
                cond.rhs_val.init_raw(sizeof(double));
            }
            else if (lhs_type == TYPE_INT && cond.rhs_val.type == TYPE_FLOAT){
                cond.rhs_val.set_int(static_cast<int>(cond.rhs_val.float_val));
                cond.rhs_val.init_raw(sizeof(int));
            }
            // BIGINT 向下兼容 INT
            else if (lhs_type == TYPE_BIGINT && cond.rhs_val.type == TYPE_INT){
                cond.rhs_val.set_bigint(static_cast<long long>(cond.rhs_val.int_val));
                cond.rhs_val.init_raw(sizeof(long long));
            }
            else if (lhs_type == TYPE_INT && cond.rhs_val.type == TYPE_BIGINT) {
                if (cond.rhs_val.bigint_val <= INT32_MAX && cond.rhs_val.bigint_val >= INT32_MIN) {
                    cond.rhs_val.set_int(static_cast<int>(cond.rhs_val.bigint_val));
                    cond.rhs_val.init_raw(sizeof(int));
                }
            }
            else if (lhs_type == TYPE_STRING && cond.rhs_val.type == TYPE_STRING) {
                // 字符串
                cond.rhs_val.init_raw(lhs_col->len);
            }
            else if (lhs_type == TYPE_DATETIME && cond.rhs_val.type == TYPE_DATETIME) {
                cond.rhs_val.init_raw(lhs_col->len);
            }
            // 题目9 日期向字符串的转换
            else if (lhs_type == TYPE_STRING && cond.rhs_val.type == TYPE_DATETIME) {
                cond.rhs_val.set_str(cond.rhs_val.datetime_val.to_string());
                cond.rhs_val.init_raw(lhs_col->len);
            }
            else {
                cond.rhs_val.init_raw(lhs_col->len);
            }
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto bigint_lit = std::dynamic_pointer_cast<ast::BigintLit>(sv_val)) {
        val.set_bigint(bigint_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else if (auto datetime_lit = std::dynamic_pointer_cast<ast::DatetimeLit>(sv_val)) {
        val.set_datetime(datetime_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
