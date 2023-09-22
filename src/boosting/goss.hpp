/*!
 * Copyright (c) 2017 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for license information.
 */
#ifndef LIGHTGBM_BOOSTING_GOSS_H_
#define LIGHTGBM_BOOSTING_GOSS_H_

#include <LightGBM/boosting.h>
#include <LightGBM/utils/array_args.h>
#include <LightGBM/utils/log.h>
#include <LightGBM/utils/openmp_wrapper.h>

#include <string>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

#include "gbdt.h"
#include "score_updater.hpp"

namespace LightGBM {

#ifdef TIMETAG
std::chrono::duration<double, std::milli> subset_time;
std::chrono::duration<double, std::milli> re_init_tree_time;
#endif

class GOSS: public GBDT {
 public:
  /*!
  * \brief Constructor
  */
  GOSS() : GBDT() {
  }

  ~GOSS() {
    #ifdef TIMETAG
    Log::Info("GOSS::subset costs %f", subset_time * 1e-3);
    Log::Info("GOSS::re_init_tree costs %f", re_init_tree_time * 1e-3);
    #endif
  }

  void Init(const Config* config, const Dataset* train_data, const ObjectiveFunction* objective_function,
            const std::vector<const Metric*>& training_metrics) override {
    GBDT::Init(config, train_data, objective_function, training_metrics);
    ResetGoss();
  }

  void ResetTrainingData(const Dataset* train_data, const ObjectiveFunction* objective_function,
                         const std::vector<const Metric*>& training_metrics) override {
    GBDT::ResetTrainingData(train_data, objective_function, training_metrics);
    ResetGoss();
  }

  void ResetConfig(const Config* config) override {
    GBDT::ResetConfig(config);
    ResetGoss();
  }

  void ResetGoss() {
      if(config_->top_rate == 0.0f) return;
    CHECK(config_->top_rate + config_->other_rate <= 1.0f);
    CHECK(config_->top_rate > 0.0f && config_->other_rate >= 0.0f);
    if (config_->bagging_freq > 0 && config_->bagging_fraction != 1.0f) {
      Log::Fatal("Cannot use bagging in GOSS");
    }
    Log::Info("Using GOSS");

    bag_data_indices_.resize(num_data_);
    tmp_indices_.resize(num_data_);
    tmp_indice_right_.resize(num_data_);
    offsets_buf_.resize(num_threads_);
    left_cnts_buf_.resize(num_threads_);
    right_cnts_buf_.resize(num_threads_);
    left_write_pos_buf_.resize(num_threads_);
    right_write_pos_buf_.resize(num_threads_);

    is_use_subset_ = false;
//    if (config_->top_rate + config_->other_rate <= 0.5) {
//      auto bag_data_cnt = static_cast<data_size_t>((config_->top_rate + config_->other_rate) * num_data_);
//      bag_data_cnt = std::max(1, bag_data_cnt);
//      tmp_subset_.reset(new Dataset(bag_data_cnt));
//      tmp_subset_->CopyFeatureMapperFrom(train_data_);
//      is_use_subset_ = true;
//    }
    // flag to not bagging first
    bag_data_cnt_ = num_data_;
  }

  data_size_t BaggingHelper(Random& cur_rand, data_size_t start, data_size_t cnt, data_size_t* buffer, data_size_t* buffer_right, int iter) {
    if (cnt <= 0) {
      return 0;
    }
    std::cout<<"in Bagging Helper"<<std::endl;
    std::vector<score_t> tmp_gradients(cnt, 0.0f);
//    std::cout<<"num_tree_per_iteration_:"<<num_tree_per_iteration_<<std::endl;
//    std::cout<<"in goss max absolute graidient:"<<*std::max_element(gradients_.begin(),gradients_.end(),abs_compare)<<std::endl;
//    std::cout<<"in goss max absolute hessians_:"<<*std::max_element(hessians_.begin(),hessians_.end(),abs_compare)<<std::endl;
//    std::cout<<"gradients:"<<std::endl;
    for (data_size_t i = 0; i < cnt; ++i) {
      for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
        size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
//        tmp_gradients[i] += std::fabs(gradients_[idx] * hessians_[idx]);
//          tmp_gradients[i] += std::fabs(gradients_[idx] / hessians_[idx]);
          tmp_gradients[i] += std::fabs(gradients_[idx]);
//          std::cout<<gradients_[idx]<<" ";
      }
    }
//    std::cout<<"in goss max tmp_gradients:"<<*std::max_element(tmp_gradients.begin(),tmp_gradients.end())<<std::endl;
    data_size_t top_k = static_cast<data_size_t>(cnt * config_->top_rate);
//    data_size_t other_k = static_cast<data_size_t>(cnt * config_->other_rate);
    top_k = std::max(1, top_k);
//    top_k = 1;
    ArrayArgs<score_t>::ArgMaxAtK(&tmp_gradients, 0, static_cast<int>(tmp_gradients.size()), top_k - 1);


//    score_t threshold = tmp_gradients[top_k - 1];
//    bool constant_pruning = true;

    score_t threshold = 1;
    if(config_->objective == std::string("regression")){
      threshold = 1;
    }
    else if (config_->objective == std::string("binary")){
      threshold = 0.5;
    }
    else
      threshold = 1;
//    threshold = 1;
//    if(constant_pruning)
//      threshold = 1;
//    else
//      threshold = std::pow(0.95,iter);

//    std::cout<<"argmax gradients 0:"<<tmp_gradients[0]<<std::endl;
//    for(int i = 0; i < top_k; i++)
//        std::cout<<tmp_gradients[i]<<" ";
//    score_t multiply = static_cast<score_t>(cnt - top_k) / other_k;


//    data_size_t bag_data_cnt =
//
    if(config_->boost_method == std::string("dpboost_bagging")){
      double base = 1 - config_->learning_rate;
      data_size_t bag_data_cnt = static_cast<data_size_t>(std::pow(base, iter_) * (1-base) / (1-std::pow(base, config_->num_iterations)) * cnt);
//      data_size_t bag_data_cnt = cnt;
      std::cout<<"bag_data_cnt:"<<bag_data_cnt<<std::endl;
//      data_size_t chosen_data_cnt =
      data_size_t cur_left_cnt = 0;
      data_size_t cur_right_cnt = 0;
      data_size_t chosen_data_left_cnt = 0;
      data_size_t cur_pass_cnt = 0;
      data_size_t chosen_data_cnt_tmp = 0;
//      std::cout<<"0"<<std::endl;
//      auto right_buffer = buffer + bag_data_cnt;
      for(data_size_t i = 0; i < cnt; i++){
        if(!already_chosen[i]){
//          std::cout<<"1"<<std::endl;
          float prob = (bag_data_cnt - cur_pass_cnt) / static_cast<float>(cnt - i - (chosen_data_cnt - chosen_data_left_cnt));
//          std::cout<<"prob:"<<prob<<std::endl;
          if(cur_rand.NextFloat() < prob){
//            std::cout<<"2"<<std::endl;
            score_t grad = 0.0f;
            for(int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id){
              size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
              grad += std::fabs(gradients_[idx]);
            }
            if(grad <= threshold){
//              std::cout<<"3"<<std::endl;
              buffer[cur_left_cnt++] = start+i;

              already_chosen[i] = 1;

              chosen_data_cnt_tmp++;
            }
            else{
              buffer_right[cur_right_cnt++] = start+i;
            }
            cur_pass_cnt++;
          }
          else{
            buffer_right[cur_right_cnt++] = start+i;
          }
        }
        else{
          chosen_data_left_cnt++;
        }
      }
      chosen_data_cnt += chosen_data_cnt_tmp;
//      chosen_data_cnt = 0;
      std::cout<<"cur_pass_cnt:"<<cur_pass_cnt<<std::endl;
      std::cout<<"cur_left_cnt:"<<cur_left_cnt<<std::endl;
      return cur_left_cnt;
    }
    else {
      data_size_t cur_left_cnt = 0;
      data_size_t cur_right_cnt = 0;
      data_size_t big_weight_cnt = 0;

      for (data_size_t i = 0; i < cnt; ++i) {
        score_t grad = 0.0f;
        for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
          size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
//        grad += std::fabs(gradients_[idx] * hessians_[idx]);
//        grad += std::fabs(gradients_[idx] / hessians_[idx]);
          grad += std::fabs(gradients_[idx]);
        }
//      if (grad >= threshold) {
//        buffer[cur_left_cnt++] = start + i;
//        ++big_weight_cnt;
//      }
        if (grad <= threshold) {
          buffer[cur_left_cnt++] = start + i;
          ++big_weight_cnt;
        } else {
//        data_size_t sampled = cur_left_cnt - big_weight_cnt;
//        data_size_t rest_need = other_k - sampled;
//        data_size_t rest_all = (cnt - i) - (top_k - big_weight_cnt);
//        double prob = (rest_need) / static_cast<double>(rest_all);
//        if (cur_rand.NextFloat() < prob) {
//          buffer[cur_left_cnt++] = start + i;
//          for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
//            size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
//            gradients_[idx] *= multiply;
//            hessians_[idx] *= multiply;
//          }
//        } else {
          buffer_right[cur_right_cnt++] = start + i;
//        }
        }
      }

//    for(int i = 0; i < cur_left_cnt; i++)
//      std::cout<<buffer[i]<<" ";
//    std::cout<<"the maximum gradient (threshold):"<<threshold<<std::endl;
//    std::cout<<"the number of gradients after sample:"<<cur_left_cnt<<" ";
      return cur_left_cnt;
    }
  }

  void Bagging(int iter) override {
    int n_threads = num_threads_;
    bag_data_cnt_ = num_data_;
    std::cout<<"num of data:"<<num_data_<<std::endl;
    // not subsample for first iterations

//    if(iter < 1) return;

//    if (iter < static_cast<int>(1.0f / config_->learning_rate)) { return; }

    const data_size_t min_inner_size = 100;


    num_threads_ = 1;
    data_size_t inner_size = (num_data_ + num_threads_ - 1) / num_threads_;
    if (inner_size < min_inner_size) { inner_size = min_inner_size; }
//    OMP_INIT_EX();

//    #pragma omp parallel for schedule(static, 1)
//    std::cout<<"num_threads:"<<num_threads_<<std::endl;
    for (int i = 0; i < num_threads_; ++i) {
//      OMP_LOOP_EX_BEGIN();
      left_cnts_buf_[i] = 0;
      right_cnts_buf_[i] = 0;
      data_size_t cur_start = i * inner_size;
      if (cur_start > num_data_) { continue; }
      data_size_t cur_cnt = inner_size;
      if (cur_start + cur_cnt > num_data_) { cur_cnt = num_data_ - cur_start; }
//      Random cur_rand(config_->bagging_seed + iter * num_threads_ + i);
        Random cur_rand(3);
      data_size_t cur_left_count = BaggingHelper(cur_rand, cur_start, cur_cnt,
                                                 tmp_indices_.data() + cur_start, tmp_indice_right_.data() + cur_start, iter);
      offsets_buf_[i] = cur_start;
      left_cnts_buf_[i] = cur_left_count;
//      std::cout<<"cur_left_count:"<<cur_left_count<<std::endl;

      right_cnts_buf_[i] = cur_cnt - cur_left_count;
//      OMP_LOOP_EX_END();
    }
//    OMP_THROW_EX();
    data_size_t left_cnt = 0;
    left_write_pos_buf_[0] = 0;
    right_write_pos_buf_[0] = 0;
    for (int i = 1; i < num_threads_; ++i) {
      left_write_pos_buf_[i] = left_write_pos_buf_[i - 1] + left_cnts_buf_[i - 1];
      right_write_pos_buf_[i] = right_write_pos_buf_[i - 1] + right_cnts_buf_[i - 1];
    }
    left_cnt = left_write_pos_buf_[num_threads_ - 1] + left_cnts_buf_[num_threads_ - 1];
//    std::cout<<"total left count:"<<left_cnt<<std::endl;
//    #pragma omp parallel for schedule(static, 1)
    for (int i = 0; i < num_threads_; ++i) {
//      OMP_LOOP_EX_BEGIN();
      if (left_cnts_buf_[i] > 0) {
        std::memcpy(bag_data_indices_.data() + left_write_pos_buf_[i],
                    tmp_indices_.data() + offsets_buf_[i], left_cnts_buf_[i] * sizeof(data_size_t));
      }
      if (right_cnts_buf_[i] > 0) {
        std::memcpy(bag_data_indices_.data() + left_cnt + right_write_pos_buf_[i],
                    tmp_indice_right_.data() + offsets_buf_[i], right_cnts_buf_[i] * sizeof(data_size_t));
      }
//      OMP_LOOP_EX_END();
    }
//    OMP_THROW_EX();
    bag_data_cnt_ = left_cnt;
//    std::cout<<"is_use_subset_:"<<is_use_subset_<<std::endl;
    // set bagging data to tree learner
    if (!is_use_subset_) {
      tree_learner_->SetBaggingData(bag_data_indices_.data(), bag_data_cnt_);
    } else {
      // get subset
      #ifdef TIMETAG
      auto start_time = std::chrono::steady_clock::now();
      #endif
      tmp_subset_->ReSize(bag_data_cnt_);
      tmp_subset_->CopySubset(train_data_, bag_data_indices_.data(), bag_data_cnt_, false);
      #ifdef TIMETAG
      subset_time += std::chrono::steady_clock::now() - start_time;
      #endif
      #ifdef TIMETAG
      start_time = std::chrono::steady_clock::now();
      #endif
      tree_learner_->ResetTrainingData(tmp_subset_.get());
      #ifdef TIMETAG
      re_init_tree_time += std::chrono::steady_clock::now() - start_time;
      #endif
    }
    num_threads_ = n_threads;
  }

 private:
  std::vector<data_size_t> tmp_indice_right_;
};

}  // namespace LightGBM
#endif   // LIGHTGBM_BOOSTING_GOSS_H_
