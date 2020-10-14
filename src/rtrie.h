/** \file
 * Реализация префиксного дерева.
 *
 * \defgroup syntax_tree Внутреннее представление РЕФАЛ программы.
 *
 * Идентификаторы (имена функциональных определений) хранятся в виде
 * префиксного тернарного дерева. Структура заполнятся при чтении исходного
 * текста постепенно, по одному символу.
 */

#pragma once

#include <assert.h>
#include <stdlib.h>
#include <wchar.h>

#include "message.h"

/**\addtogroup syntax_tree
 * \{
 */

typedef unsigned rtrie_index;

struct rtrie_node;

/**
 * Описатель префиксного дерева.
 */
struct refal_trie {
   struct rtrie_node *n;   ///< Массив узлов.
   rtrie_index size;       ///< Текущий размер.
   rtrie_index free;       ///< Первый свободный элемент.
};

/**
 * Соответствующее префиксу (ключу) значение.
 */
struct rtrie_val {
   rf_type     tag :4;     ///< Тип содержимого (`rf_machine_code`).
   rf_index    value:28;   ///< Индекс (ячейки РЕФАЛ-машины или таблице импорта).
};

/**
 * Узел.
 * Дочерние элементы определяются индексами, а не указателями. Это позволяет
 * хранить структуру в произвольных адресах и запоминающих устройствах.
 * Нулевой индекс сигнализирует отсутствие ветви (на 0й узел некому ссылаться).
 */
struct rtrie_node {
   wchar_t     chr;        ///< Текущий символ последовательности.
   rtrie_index next;       ///< Узел со следующим символом последовательности.
   rtrie_index left;       ///< Меньший по значению символ.
   rtrie_index right;      ///< Больший по значению символ.
   struct rtrie_val val;   ///< Соответствующее префиксу значение.
};

/**
 * Резервирует память для хранения внутреннего представления РЕФАЛ программы.
 * \result Инициализированная  структура `struct refal_trie`.
 */
static inline
struct refal_trie rtrie_alloc(
      rtrie_index size)    ///< Предполагаемый размер (в узлах).
{
   struct refal_trie rt = {
      .n = calloc(size, sizeof(struct rtrie_node)),
      .size = size,
   };
   return rt;
}

/**
 * Проверяет состояние структуры.
 *
 * \result Ненулевой результат, если память распределена.
 */
static inline
void *rtrie_check(
      struct refal_trie    *rt,
      struct refal_message *status)
{
   assert(rt);
   if (status && !rt->n) {
      critical_error(status, "недостаточно памяти", rt->free, rt->size);
   }
   return rt->n;
}

/**
 * Освобождает занятую память.
 */
static inline
void rtrie_free(
      struct refal_trie *rtrie)
{
   free(rtrie->n);
}

/**
 * Размещает новый узел символа \c chr.
 * \result Индекс узла (вызывающая сторона должна связать его с предком).
 */
static inline
rtrie_index rtrie_new_node(
      struct refal_trie *restrict rt,
      wchar_t           chr)
{
   assert(rt);
   assert(rt->size);
   assert(chr);

   rtrie_index node = rt->free++;
   assert(node < rt->size);
   rt->n[node] = (struct rtrie_node) { .chr = chr };
   return node;
}

/**
 * Находит (добавляя при отсутствии) узел символа \c chr, начиная поиск с \c idx.
 * \result Индекс узла.
 */
static inline
rtrie_index rtrie_insert_at(
      struct refal_trie *restrict rt,
      rtrie_index       idx,           ///< Начальный узел поиска.
      wchar_t           chr)           ///< Текущий символ имени.
{
   // Если значение совпадает с символом, значит узел найден;
   // иначе проверяем соседей.
   // В случае пустого дерева выполняется условие chr > rt->n[0].chr
   // и rt->n[0].right присваивается индекс нового узла, равный 0.
   while (1) {
      if (chr == rt->n[idx].chr) {
         return idx;
      } else if (chr > rt->n[idx].chr) {
         if (rt->n[idx].right) {
            idx = rt->n[idx].right;
         } else {
            rt->n[idx].right = rtrie_new_node(rt, chr);
            return rt->n[idx].right;
         }
      } else /* if (chr < rt->n[idx].chr) */ {
         if (rt->n[idx].left) {
            idx = rt->n[idx].left;
         } else {
            rt->n[idx].left = rtrie_new_node(rt, chr);
            return rt->n[idx].left;
         }
      }
   }
}

/**
 * Находит (добавляя при отсутствии) узел с первым символом идентификатора.
 * \result Индекс узла.
 */
static inline
rtrie_index rtrie_insert_first(
      struct refal_trie *restrict rt,
      wchar_t           chr)           ///< Первый символ имени.
{
   return rtrie_insert_at(rt, 0, chr);
}

/**
 * Находит (добавляя при отсутствии) узел со следующим символом идентификатора.
 *
 * \result Индекс узла.
 */
static inline
rtrie_index rtrie_insert_next(
      struct refal_trie *restrict rt,
      rtrie_index       idx,           ///< Результат предыдущего поиска.
      wchar_t           chr)           ///< Текущий символ имени.
{
   if (rt->n[idx].next) {
      return rtrie_insert_at(rt, rt->n[idx].next, chr);
   }
   rt->n[idx].next = rtrie_new_node(rt, chr);
   return rt->n[idx].next;
}

/**
 * Находит соответствующее последовательности символов `prefix` значение
 * `struct rtrie_val` и возвращает его.
 */
static inline
struct rtrie_val rtrie_get_value(
      const struct refal_trie *restrict rt,
      const char              *restrict prefix)
{
   assert(rt);
   assert(prefix);
   // Вернёт 0 по условию if (chr < rt->n[idx].chr), но приходить не должно.
   assert(*prefix);

   rtrie_index idx = 0;
   wchar_t chr = *prefix++;
   while (1) {
      if (chr == rt->n[idx].chr) {
         if (*prefix) {
            chr = *prefix++;
            idx = rt->n[idx].next;
         } else {
            return rt->n[idx].val;
         }
      } else if (chr > rt->n[idx].chr) {
         if (!(idx = rt->n[idx].right))
            return (struct rtrie_val) { };
      } else /* if (chr < rt->n[idx].chr) */ {
         if (!(idx = rt->n[idx].left))
            return (struct rtrie_val) { };
      }
   }
}

/**\}*/

