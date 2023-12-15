#ifndef GLIDER_PREDICTOR_H
#define GLIDER_PREDICTOR_H

using namespace std;
#include <map>
#include <vector>
#include <numeric>
#include "optgen.h"
#include "helper_function.h"

#define MAX_PCMAP 31
#define PCMAP_SIZE 16 // 先假定为16

// 常量定义
#define GLIDER_THRESHOLD_HIGH 60 // 预测阈值
#define GLIDER_THRESHOLD_LOW 0

enum class Prediction
{
    High,
    Medium,
    Low
};

class IntegerSVM
{
private:
    vector<int> weights;

public:
    IntegerSVM(int num_weights) { weights.resize(num_weights, 0); }

    void update_weights(const vector<uint64_t> &indices, int should_cache, int thresholds, int learning_rate = 1, int lambda = 1, int regularization_threshold = 5)
    {
        // 计算权重总和
        int weight_sum = accumulate(indices.begin(), indices.end(), 0, [this](int sum, int idx)
                                    { return sum + this->weights[idx]; });

        // 检查是否超过阈值
        if (abs(weight_sum) >= thresholds)
        {
            return; // 如果超过任何阈值，则不更新权重
        }

        // 根据 OPTgen 决策更新权重
        for (auto idx : indices)
        {
            weights[idx] += learning_rate * should_cache;
        }

        // 应用阶跃式正则化
        for (auto &weight : weights)
        {
            if (abs(weight) > regularization_threshold)
            {
                if (weight > 0)
                {
                    weight -= lambda;
                }
                else
                {
                    weight += lambda;
                }
            }
        }
    }

    // 根据pchr索引，计算权重总和
    int calculate_weights(vector<uint64_t> pchr)
    {
        int weight_sum = 0;
        for (uint64_t pc : pchr)
        {
            weight_sum += weights[pc];
        }
        return weight_sum;
    }

    // 计算全体权重总和
    int calculate_sum() const
    {
        return accumulate(weights.begin(), weights.end(), 0);
    }

    Prediction predict(const vector<int> &indices)
    {
        // 计算预测值
        int prediction = accumulate(indices.begin(), indices.end(), 0, [this](int sum, int idx)
                                    { return sum + this->weights[idx]; });
        return prediction >= 60 ? Prediction::High : prediction >= 0 ? Prediction::Medium
                                                                     : Prediction::Low;
    }

    // 打印权重，用于输出调试
    void print_weights() const
    {
        for (int weight : weights)
        {
            cout << weight << " ";
        }
        cout << endl;
    }
};

class Glider_Predictor
{
private:
    vector<uint64_t> pchr;                                          // PCHR历史记录
    const int k_sparse = 5;                                         // PCHR的最大长度
    const vector<int> dynamic_thresholds = {0, 30, 100, 300, 3000}; // 动态阈值集合
    vector<IntegerSVM> isvms;

public:
    // 构造函数，初始化OPTgen和PCHR
    Glider_Predictor()
    {
        pchr.resize(k_sparse, 0);

        for (int i = 0; i < PCMAP_SIZE; i++)
        {
            isvms.push_back(IntegerSVM(PCMAP_SIZE));
        }
    }

    // 动态调整阈值
    int select_dynamic_threshold()
    { // 根据当前所有isvm的权重总和，选择合适的阈值
        int weight_sum = 0;
        for (const auto &svm : isvms)
        {
            weight_sum += svm.calculate_sum();
        }
        // 根据权重总和，选择合适的阈值
        size_t index;
        if (weight_sum < 500)
        {
            index = 4;
        }
        else if (weight_sum < 1000)
        {
            index = 3;
        }
        else if (weight_sum < 2000)
        {
            index = 2;
        }
        else if (weight_sum < 4000)
        {
            index = 1;
        }
        else
        {
            index = 0;
        }
        return dynamic_thresholds[index];
    }

    // 返回预测结果
    Prediction get_prediction(uint64_t PC)
    {
        // 更新PCHR历史记录，后面push一下，前面erase掉一个
        uint64_t encoded_pc = CRC(PC) % PCMAP_SIZE; // 取CRC散列后mod16
        pchr.push_back(encoded_pc);
        if (pchr.size() > (size_t)k_sparse)
        {
            pchr.erase(pchr.begin());
        }

        int weight_sum = isvms[encoded_pc].calculate_weights(pchr);

        if (weight_sum >= GLIDER_THRESHOLD_HIGH)
        { // 高优先级预测
            return Prediction::High;
        }
        else if (weight_sum < GLIDER_THRESHOLD_LOW)
        { // 低优先级预测
            return Prediction::Low;
        }
        return Prediction::Medium; // 中等优先级预测
    }

    void increase(uint64_t PC)
    {
        uint64_t encoded_pc = CRC(PC) % PCMAP_SIZE;

        // 选择合适的动态阈值
        int selected_threshold = select_dynamic_threshold();

        isvms[encoded_pc].update_weights(pchr, 1, selected_threshold);
    }

    void decrease(uint64_t PC)
    {
        uint64_t encoded_pc = CRC(PC) % PCMAP_SIZE;

        // 选择合适的动态阈值
        int selected_threshold = select_dynamic_threshold();

        isvms[encoded_pc].update_weights(pchr, -1, selected_threshold);
    }

    // 打印isvms的所有权重，用于输出调试
    void print_all_weights() const
    {
        cout << "Printing all SVM weights:" << endl;
        int svm_index = 1;
        for (const auto &svm : isvms)
        {
            cout << "SVM #" << svm_index << ": ";
            svm.print_weights();
            svm_index++;
        }
        cout << "----------" << endl;
    }
};

#endif