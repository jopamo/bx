#ifndef BX_MIRA_GITHUB_INTERNAL_H
#define BX_MIRA_GITHUB_INTERNAL_H
#include "repository.h"
#define MIRA_GITHUB_API_ROOT "https://api.github.com"
typedef struct {
    const char* jq_program;
    const char* bearer_token;
    const char* bearer_token_file;
    const char** operands;
    int operand_count;
    bool json;
    bool no_proxy;
    bool recursive;
    MiraRepositoryOptions repository;
} MiraGithubArguments;


bool bx_mira_github_repo_is_valid(const char* repository);
struct bx_fetch_config* bx_mira_github_config(const MiraGithubArguments* arguments, const char* url, const char* output_path);
int bx_mira_github_tree(const MiraGithubArguments* arguments);
#endif
