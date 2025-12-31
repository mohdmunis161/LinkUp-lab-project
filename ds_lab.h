
#ifndef DS_LAB_H
#define DS_LAB_H

#define HASH_SIZE 1000
struct modified_content_node {
    int content_id;
    std::string content;
    int timestamp;
    modified_content_node* next;
};

// Demo-only globals (not used in main logic)
extern modified_content_node* content_hash[HASH_SIZE];
extern modified_content_node* modified_content_root;

int get_hash_index(int key);
void insert_hash_nodes(modified_content_node* root);
modified_content_node* search_hash_node(int content_id);

#include <string>
#include <iostream>

struct content_node {
    int content_id;
    std::string content;
    content_node* next;
    content_node(int id, const std::string& c) : content_id(id), content(c), next(nullptr) {}
};

struct char_node {
    int content_id;
    char_node* next_neabor_instr;
    char_node* next;
    char_node(int id) : content_id(id), next_neabor_instr(nullptr), next(nullptr) {}
};

struct content_id_node {
    int content_id;
    content_id_node* next;
    content_id_node(int id) : content_id(id), next(nullptr) {}
};

void create_seq_search_multilist(content_node* content_list_root, char_node* search_chars_arr[128]);
void insert_unique(content_id_node*& head, int id);
content_id_node* seq_search_fallback(content_node* root, const std::string& seq);
void display_results(content_id_node* head);

void test_content(content_node* root);

#endif // DS_LAB_H
