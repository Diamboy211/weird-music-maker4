#include <stdlib.h>
#include <string.h>
#include "list.h"

void list_create(List *list)
{
	memset(list, 0, sizeof(List));
}

ListNode *list_insert_after(List *list, ListNode *node, const uint8_t *data)
{
	ListNode *e = malloc(sizeof(ListNode));
	if (e == NULL) return NULL;
	memcpy(e->e, data, 4);
	list->length++;
	if (list->length == 1)
	{
		e->prev      = NULL;
		e->next      = NULL;
		list->front  = e;
		list->back   = e;
		return e;
	}
	e->prev = node;
	e->next = node->next;
	node->next = e;
	if (node != list->back)
		e->next->prev = e;
	else list->back = e;
	return e;
}

ListNode *list_insert_before(List *list, ListNode *node, const uint8_t *data)
{
	ListNode *e = malloc(sizeof(ListNode));
	if (e == NULL) return NULL;
	memcpy(e->e, data, 4);
	list->length++;
	if (list->length == 1)
	{
		e->prev      = NULL;
		e->next      = NULL;
		list->front  = e;
		list->back   = e;
		return e;
	}
	e->prev = node->prev;
	e->next = node;
	node->prev = e;
	if (node != list->front)
		e->prev->next = e;
	else list->front = e;
	return e;
}

void list_remove(List *list, ListNode *node)
{
	list->length--;
	if (list->length == 0)
	{
		list->front = NULL;
		list->back  = NULL;
	}
	else if (node == list->front)
	{
		node->next->prev = NULL;
		list->front      = node->next;
	}
	else if (node == list->back)
	{
		node->prev->next = NULL;
		list->back       = node->prev;
	}
	else
	{
		node->prev->next = node->next;
		node->next->prev = node->prev;
	}
	free(node);
	return;
}

void list_clear(List *list)
{
	if (list->length == 0) return;
	ListNode *it = list->front;
	while (it)
	{
		ListNode *next = it->next;
		free(it);
		it = next;
	}
	list->length = 0;
	list->front  = NULL;
	list->back   = NULL;
}

void list_destroy(List *list)
{
	list_clear(list);
}

void list_swap(List *list1, List *list2)
{
	int tl = list1->length;
	list1->length = list2->length;
	list2->length = tl;
	ListNode *tp = list1->front;
	list1->front = list2->front;
	list2->front = tp;
	tp = list1->back;
	list1->back = list2->back;
	list2->back = tp;
}
