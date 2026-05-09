#ifndef COMMON_INTERVIEW_TYPES
#define COMMON_INTERVIEW_TYPES

#include <string>
#include <vector>

// 
struct Question {
    int id =  0; //  题目编号
    std::string text; // 题目内容
    bool is_followup = false; // 是否追问
    int parent_question_id = -1; // 是追问题，则表示对应的主问题id。不是，默认-1
};

struct AnswerRecord {
    int question_id = 0; // 当前回答对应问题的题目id

    std::string question_text; // 当前回答对应的题目文本
    std::string answer_text; // 用户输入的回答内容
    int score = 0; // 本次回答的评分
    bool need_followup = false; // 当前回答后，系统是否还需要继续追问

    std::string followup_question; // 如果需要追问，这里保存追问内容
    // 这条记录是不是“追问题的回答记录”，
    // false 表示主问题回答
    // true 表示追问题回答
    bool is_followup_answer = false;  
};

// LLM 对一次回答的评估结果
// InterviewSession在提交回答后，会拿到这个结果
struct EvaluateResult {
    int score = 0; // 评分结果
    
    bool need_followup = false; // 是否需要追问
    std::string followup_question; // 如果要追问，追问题目

    std::string feedback; // 对本次回答的简短反馈
};

struct InterviewReport {
    std::vector<AnswerRecord> records; // 正常面试的所有回答记录

    std::string summary; // 最终总结内容
    int total_score = 0; // 总分
};

#endif // COMMON_INTERVIEW_TYPES