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

static_assert(sizeof(struct rf_id) == sizeof(uint32_t), "размеры должны соответствовать");

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
   struct rf_id val;       ///< Соответствующее префиксу значение.
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
   rt->size = rt->n ? size : 0;
   rt->free = 0;
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
   refal_free(rtrie->n, rtrie->size * sizeof(struct rtrie_node));
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
      struct refal_trie *rt,
      wchar_t           chr)
{
   assert(rt);
   assert(rt->size);
   assert(chr);

   rtrie_index node = rt->free++;
   assert(!(node > rt->size));
   if (node == rt->size) {
      size_t size = rt->size * sizeof(struct rtrie_node);
      //TODO нет памяти.
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
      struct refal_trie *rt,  ///< Таблица символов.
      rtrie_index       idx,  ///< Начальный узел поиска.
      wchar_t           chr)  ///< Текущий символ имени.
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
            rtrie_index right = rtrie_new_node(rt, chr);
            // Sequence point, в одну строку UB (§6.5/2)
            rt->n[idx].right = right;
            return right;
         }
      } else /* if (chr < rt->n[idx].chr) */ {
         if (rt->n[idx].left) {
            idx = rt->n[idx].left;
         } else {
            rtrie_index left = rtrie_new_node(rt, chr);
            rt->n[idx].left = left;
            return left;
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
      struct refal_trie *rt,  ///< Таблица символов.
      wchar_t           chr)  ///< Первый символ имени.
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
      struct refal_trie *rt,  ///< Таблица символов.
      rtrie_index       idx,  ///< Результат предыдущего поиска.
      wchar_t           chr)  ///< Текущий символ имени.
{
   if (rt->n[idx].next) {
      return rtrie_insert_at(rt, rt->n[idx].next, chr);
   }
   rtrie_index next = rtrie_new_node(rt, chr);
   // Sequence point, в одну строку порядок вычислений не определён (§6.5/2)
   rt->n[idx].next = next;
   return next;
}

/**
 * Ищет узел символа \c chr, начиная поиск с \c idx.
 * \result Индекс узла либо -1 в случае отсутствия.
 */
static inline
rtrie_index rtrie_find_at(
      const struct refal_trie *rt,  ///< Таблица символов.
      rtrie_index             idx,  ///< Начальный узел поиска.
      wchar_t                 chr)  ///< Текущий символ имени.
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
      const struct refal_trie *rt,  ///< Таблица символов.
      wchar_t                 chr)  ///< Первый символ имени.
{
   return rtrie_find_at(rt, 0, chr);
}

/**
 * Ищет узел со следующим символом идентификатора.
 * \result Индекс узла либо -1 в случае отсутствия.
 */
static inline
rtrie_index rtrie_find_next(
      const struct refal_trie *rt,  ///< Таблица символов.
      rtrie_index             idx,  ///< Результат предыдущего поиска.
      wchar_t                 chr)  ///< Первый символ имени.
{
   return idx < 0 ? idx
                  : rt->n[idx].next ? rtrie_find_at(rt, rt->n[idx].next, chr)
                                    : -1;
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
struct rf_id rtrie_get_value(
      const struct refal_trie *rt,
      const char              *prefix)
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
            return (struct rf_id) { rf_id_undefined };
      } else /* if (chr < rt->n[idx].chr) */ {
         if (!(idx = rt->n[idx].left))
            return (struct rf_id) { rf_id_undefined };
      }
   }
}

/**
 * Ищет в поле зрения идентификатор с типом tag1 или tag2
 * и возвращает определяемый им описатель функции,
 * либо пустышку { rft_undefined, 0 } при отсутствии искомого.
 * Содержимое структурных скобок пропускается, в случае выхода индекса
 * закрывающей скобки за допустимые пределы возвращается { rft_undefined, -1 }
 *
 * Идентификатор может быть явным опкодом с rf_identifier
 * либо определяться последовательностью rf_char - в таком случае проверяется
 * вся строка до первого отличного от rf_char кода.
 * Если найден, удаляется из поля зрения.
 */
static inline
struct rf_id rtrie_find_value_by_tags(
      const struct refal_trie *rt,
      enum rf_id_type         tag1,
      enum rf_id_type         tag2,
      struct refal_vm         *vm,
      rf_index                prev,
      rf_index                next)
{
   struct rf_id function;
   for (rf_index n, id = vm->u[prev].next; id != next; id = n) {
      n = vm->u[id].next;
      switch (vm->u[id].op) {
      case rf_identifier:
         function = vm->u[id].id;
         if (function.tag == tag1 || function.tag == tag2) {
found:      rf_free_evar(vm, vm->u[id].prev, n);
            return function;
         } else if (function.tag == rf_id_undefined) {// не должно возникать при трансляции.
            return function;
         }
         continue;
      case rf_char: ;
         // Просматриваем последовательность символов до конца.
         // Параллельно производится поиск в дереве, если возможен.
         // Пробел может следовать после имени модуля и вызывает
         // поиск в отдельном пространство имён.
         rtrie_index idx = rtrie_find_first(vm->rt, vm->u[id].chr);
         wchar_t pc = L'\0';
         for (n = vm->u[id].next ; n != next && vm->u[n].op == rf_char; n = vm->u[n].next)
            if (!(idx < 0)) {
               idx = pc == L' ' ? rtrie_find_at(vm->rt, idx, vm->u[n].chr)
                           : rtrie_find_next(vm->rt, idx, vm->u[n].chr);
               pc = vm->u[n].chr;
            }
         // Если идентификатор "найден", но неопределён,
         // значит это часть другого. Считаем его обычным текстом.
         // Так же пропускаем и неподходящие.
         if (!(idx < 0)) {
            function = vm->rt->n[idx].val;
            if (function.tag == tag1 || function.tag == tag2)
               goto found;
         }
         continue;
      case rf_opening_bracket:
         id = vm->u[id].link;
         if (!(id < vm->size)) {
            return (struct rf_id) { rf_id_undefined, -1 };
         }
         n = vm->u[id].next;
         continue;
      default:
         continue;
      }
   }
   return (struct rf_id) { rf_id_undefined, 0 };
}


/**\}*/
/**\}*/
