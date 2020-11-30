/**\file
 * \brief Реализация префиксного дерева.
 *
 * \addtogroup translator
 * \{
 * \addtogroup syntax_tree Таблица символов.
 *
 * Идентификаторы (имена функциональных определений) хранятся в виде
 * префиксного тернарного дерева. Структура заполнятся при чтении исходного
 * текста постепенно, по одному символу.
 * \{
 */

#pragma once

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

#include "refal.h"

/**
 * Индекс для адресации ячеек массива, где хранятся префиксы.
 * Отрицательные значения не действительны.
 */
typedef int rtrie_index;

struct rtrie_node;

/**
 * Описатель префиксного дерева.
 */
struct refal_trie {
   struct rtrie_node *n;   ///< Массив узлов.
   rtrie_index size;       ///< Текущий размер.
   rtrie_index free;       ///< Первый свободный элемент.
};

typedef enum rtrie_type {
   rft_undefined,
   rft_machine_code,    ///< Ссылка на машинный код (функцию).
   rft_byte_code,       ///< Ссылка на функцию в РЕФАЛ-машине.
   rft_enum,            ///< Пустая функция (ENUM в Refal-05) или имя модуля.
} rtrie_type;

/**
 * Соответствующее префиксу (ключу) значение.
 */
struct rtrie_val {
   rtrie_type  tag :4;     ///< Тип содержимого.
   rf_index    value:28;   ///< Индекс (ячейки РЕФАЛ-машины или таблице импорта).
};

static_assert(sizeof(struct rtrie_val) == sizeof(uint32_t), "размеры должны соответствовать");

static inline
struct rtrie_val rtrie_val_from_raw(uint32_t raw)
{
   union { struct rtrie_val val; uint32_t raw; } u;
   u.raw = raw;
   return u.val;
}

static inline
uint32_t rtrie_val_to_raw(struct rtrie_val val)
{
   union { struct rtrie_val val; uint32_t raw; } u;
   u.val = val;
   return u.raw;
}


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
 * \result Ненулевое значение в случае успеха.
 */
static inline
void *rtrie_alloc(
      struct refal_trie *rt,  ///< Структура для инициализации
      rtrie_index size)       ///< Предполагаемый размер (в узлах).
{
   rt->n = refal_malloc(size * sizeof(struct rtrie_node));
   if (rt->n) {
      rt->size = size;
   };
   return rt->n;
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
      critical_error(status, "недостаточно памяти для словаря", -errno, rt->size);
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
   refal_free(rtrie->n, rtrie->size);
   rtrie->n = 0;
   rtrie->size = 0;
   rtrie->free = 0;
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
   assert(!(node > rt->size));
   if (node == rt->size) {
      size_t size = rt->size * sizeof(struct rtrie_node);
      rt->n = refal_realloc(rt->n, size, 2 * size);
      rt->size *= 2;
   }
   rt->n[node] = (struct rtrie_node) { .chr = chr };
   return node;
}

/**
 * Находит (добавляя при отсутствии) узел символа \c chr, начиная поиск с \c idx.
 * \result Индекс узла.
 */
static inline
rtrie_index rtrie_insert_at(
      struct refal_trie *restrict rt,  ///< Таблица символов.
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
      struct refal_trie *restrict rt,  ///< Таблица символов.
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
      struct refal_trie *restrict rt,  ///< Таблица символов.
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
 * Ищет узел символа \c chr, начиная поиск с \c idx.
 * \result Индекс узла либо -1 в случае отсутствия.
 */
static inline
rtrie_index rtrie_find_at(
      struct refal_trie *restrict rt,  ///< Таблица символов.
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
         if (!(idx = rt->n[idx].right))
            return -1;
      } else /* if (chr < rt->n[idx].chr) */ {
         if (!(idx = rt->n[idx].left))
            return -1;
      }
   }
}

/**
 * Ищет узел с первым символом идентификатора.
 * \result Индекс узла либо -1 в случае отсутствия.
 */
static inline
rtrie_index rtrie_find_first(
      struct refal_trie *restrict rt,  ///< Таблица символов.
      wchar_t           chr)           ///< Первый символ имени.
{
   return rtrie_find_at(rt, 0, chr);
}

/**
 * Ищет узел со следующим символом идентификатора.
 * \result Индекс узла либо -1 в случае отсутствия.
 */
static inline
rtrie_index rtrie_find_next(
      struct refal_trie *restrict rt,  ///< Таблица символов.
      rtrie_index       idx,           ///< Результат предыдущего поиска.
      wchar_t           chr)           ///< Первый символ имени.
{
   return idx < 0 ? idx : rtrie_find_at(rt, rt->n[idx].next, chr);
}

static inline
wchar_t decode_utf8(
      const char  *restrict *str_ptr)
{
   wchar_t chr = 0;
   unsigned more = 0;
   const unsigned char *restrict str = (const unsigned char*)*str_ptr;
   unsigned char octet = *str++;
   switch (octet) {
   case 0x00 ... 0x7f:
      chr = octet;
      break;
   case 0xc0 ... 0xdf:
      more = 1;
      chr = 0x1f & octet;
      break;
   case 0xe0 ... 0xef:
      more = 2;
      chr = 0x0f & octet;
      break;
   case 0xf0 ... 0xf4:
      more = 3;
      chr = 0x03 & octet;
      break;
   default:
      assert(0);
      break;
   }
   while (more-- && (octet = *str++)) {
      chr = (chr << 6) | (0x3f & octet);
   }
   *str_ptr = (const char*)str;
   return chr;
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
   wchar_t chr = decode_utf8(&prefix);
   while (1) {
      if (chr == rt->n[idx].chr) {
         if (*prefix) {
            chr = decode_utf8(&prefix);
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
/**\}*/
