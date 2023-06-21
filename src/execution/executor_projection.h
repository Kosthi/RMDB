/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    // prev_ 实际是 seq_scan 执行器
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;                  

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        int curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }

    void nextTuple() override { prev_->nextTuple(); }

    std::unique_ptr<RmRecord> Next() override {
        // 得到前一个字段
        auto& prev_cols = prev_->cols();
        // 得到前一个的记录
        auto prev_record = prev_->Next();
        // 当前要投影的字段
        auto& proj_cols = cols_;
        // 当前要显示的记录
        auto proj_record = std::make_unique<RmRecord>(len_);

        for (size_t proj_idx = 0; proj_idx < proj_cols.size(); proj_idx++) {
            size_t prev_idx = sel_idxs_[proj_idx];
            auto& prev_col = prev_cols[prev_idx];
            auto& proj_col = proj_cols[proj_idx];
            memcpy(proj_record->data + proj_col.offset, prev_record->data + prev_col.offset, proj_col.len);
        }
        return proj_record;
    }

    Rid &rid() override { return _abstract_rid; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return prev_->is_end(); }
};