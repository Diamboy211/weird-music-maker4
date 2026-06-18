#pragma once

#include <stdint.h>

typedef struct ListNode
{
	struct ListNode *prev, *next;
	uint8_t e[4];
} ListNode;

typedef struct
{
	ListNode *front, *back;
	int length;
} List;

void list_create(List *list);
ListNode *list_insert_after(List *list, ListNode *node, const uint8_t *data);
ListNode *list_insert_before(List *list, ListNode *node, const uint8_t *data);
void list_remove(List *list, ListNode *node);
void list_clear(List *list);
void list_destroy(List *list);
void list_swap(List *list1, List *list2);
