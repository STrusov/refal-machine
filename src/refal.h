/**\defgroup refal Язык РЕФАЛ.
 * \addtogroup refal
 * \{
 * ## О языке. ##
 *
 * Существует три общепринятых варианта формализации алгоритмов:
 * - Машина Тьюринга (реализуется императивными языками);
 * - Стрелка Чёрча (реализуется функциональными языками);
 * - Нормальный алгорифм Маркова (НАМ).
 *
 * Язык РЕкурсивных Формул АЛгоритмический натуральным образом реализует НАМ.
 *
 * \defgroup refal-syntax Синтаксис
 * \}
 *
 * \file
 * \brief Основной интерфейс РЕФАЛ-машины.
 *
 * Описание структур данных и имплементация вспомогательных функций,
 * в совокупности образующих РЕФАЛ-машину, способную взаимодействовать
 * с реализованными на языках C и C++ (под)программами.
 *
 * \mainpage
 * \copydoc refal
 * \copydoc refal-syntax
 * \ref Синтаксис
 *
 * Описание основано на публикации
 * «РЕФАЛ-5. Руководство по программированию и справочник» Турчин В.Ф.
 * С изменениями.
 */

#pragma once

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "message.h"

/**\addtogroup internal Внутреннее устройство РЕФАЛ-машины.
 * \{
 *
 * ## Принцип действия. ##
 *
 * РЕФАЛ-машина обрабатывает _поле зрения_, представленное совокупностью ячеек,
 * физически расположенных в массиве (изменяющем размер по необходимости).
 * Функции РЕФАЛ-машины модифицируют поле зрения, производя вставку
 * и удаление элементов в произвольное место. Вызов функции называется _шаг_.
 *
 * Поле программы для интерпретатора хранится в одном массиве с полем зрения.
 */

/**
 * Тип данных, хранящихся в ячейке памяти РЕФАЛ-машины.
 */
typedef enum rf_type {
   rf_undefined,        ///< Пустая ячейка (может хранить данные транслятора).
   rf_char,             ///< Символ (одна буква в кодировке UTF-8).
   rf_number,           ///< Целое число.
   rf_atom,             ///< Идентификатор (рассматривается целиком).
   rf_opening_bracket,  ///< Открывающая скобка.
   rf_closing_bracket,  ///< Закрывающая скобка.
   // Команды интерпретатора.
   rf_sentence,         ///< Начало предложения. Содержит ссылку на следующее.
   rf_equal,            ///< Разделяет выражение-образец от общего выражения.
   rf_open_function,    ///< Открывающая вычислительная скобка <
   rf_execute,          ///< Закрывающая вычислительная скобка >
   rf_identifier,       ///< Идентификатор (функция) // TODO rf_atom
   rf_svar,             ///< s-переменная (односимвольная).
   rf_tvar,             ///< t-переменная (s- либо выражение в скобках).
   rf_evar,             ///< e-переменная (произвольное количество элементов.
   rf_complete,         ///< Общее выражение завершено.
} rf_type;

static_assert(rf_complete < 1<<4, "Значение хранится в 4-х разрядах.");

/**
 * Адресует ячейки памяти РЕФАЛ-машины.
 */
typedef uint32_t rf_index;

enum { rf_index_max = (1 << 28) - 1 };

/**
 * Целочисленное значение в ячейке.
 */
typedef long rf_int;

/**
 * Ячейка памяти РЕФАЛ-машины.
 *
 * С целью уменьшить число операций копирования, ячейки логически объединены
 * в двусвязный список. Вместо указателей используются индексы — такое решение
 * позволяет уменьшить размер ячейки с 32-х (24-х) до 16-ти байт, а так же
 * сериализовать Поле Зрения или его часть без модификаций.
 * Размер индекса позволяет адресовать 256М (268435456) ячеек (4Гб памяти).
 * В младших 4-х разрядах хранится тег, позволяя при адресации ячеек заменить
 * умножение (LEA масштабирует индекс максимум на 8) обнулением (логическое И).
 * Не ясно, целесообразно (и возможно!) ли сократить размер ячейки до 8-ми байт,
 * поскольку приходится хранить тип данных (технически, тег можно разместить в
 * младших разрядах хранимого значения: для указателей на выровненные данные они
 * равны 0, а размер целого числа, например, в OCaml, сокращён на 1 бит).
 */
typedef struct rf_cell {
   union {
      uint64_t    data;    ///< Используется для сравнения.
      rf_int      num;     ///< Число.
      wchar_t     chr;     ///< Символ (буква).
      const char *atom;    ///< Идентификатор, UTF-8.
      rf_index    link;    ///< Узел в графе.
   };
   rf_type     tag :4;     ///< Тип содержимого.
   rf_index    prev:28;    ///< Индекс предыдущей ячейки.
   rf_index    tag2:4;     ///< Вспомогательный тип.
   rf_index    next:28;    ///< Индекс последующей ячейки.
} rf_cell;

/**
 * Описатель РЕФАЛ-машины.
 *
 * Всякое подвыражение поля зрения находится *между* ячейками, адресуемыми
 * индексами `prev` и `next`:

        [prev].next -> [[prev].next].next -> [...next].next -> [[next].prev]
        [[prev].next] <- [...prev].prev <- [[next].prev].prev <- [next].prev

 * `free` указывает на свободные ячейки списка, куда можно размещать временные
 * данные, после чего связывать сформированные части списка с произвольной
 * частью подвыражения (операция вставки).
 */
struct refal_vm {
   rf_cell     *u;   ///< Массив, содержащий ячейки.
   rf_index    size; ///< Размер массива.
   rf_index    free; ///< Первый свободный элемент.

   /// Адрес таблицы функций в машинном коде.
   const struct refal_import_descriptor *library;
   /// Количество функций в машинном коде.
   unsigned    library_size;;
};


/**\addtogroup mem Управление памятью РЕФАЛ-машины.
 *
 * Определение функций является задачей клиентского кода. В простейшем случае
 * должно быть достаточно одноимённых функций стандартной библиотеки.
 * realoc() в Linux использует перемещение страниц посредством mremap().
 * Так же возможно избежать копирования в NT, создав отображение с SEC_RESERVE.
 * \{
 */
/**
 * Запрашивает память у системы.
 * \param size требуемый объём памяти.
 * \result начальный адрес распределённого блока.
 */
void *refal_malloc(size_t size);

/**
 * Изменяет (увеличивает) размер запрошенной ранее памяти.
 * \param ptr начальный адрес памяти (результат `refal_malloc()`).
 * \param old_size ранее распределённый объём памяти.
 * \param new_size требуемый объём памяти.
 */
void *refal_realloc(void *ptr, size_t old_size, size_t new_size);

/**
 * Возвращает память системе.
 * \param size ранее распределённый объём памяти.
 * \param ptr начальный адрес памяти (результат `refal_malloc()`).
 */
void refal_free(void *ptr, size_t size);
/** \}*/

/**
 * Резервирует память для хранения поля зрения РЕФАЛ программы.
 * Связывает ячейки массива в список.
 * \result Ненулевое значение в случае успеха.
 */
static inline
void *refal_vm_init(
      struct refal_vm   *vm,  ///< Структура для инициализации.
      rf_index size)          ///< Предполагаемый размер (в ячейках).
{
   vm->u = refal_malloc(size * sizeof(rf_cell));
   if (vm->u) {
      vm->size = size;
      // 0-я ячейка зарезервирована:
      // - 0 в поле next указывает, что следует достроить список;
      // - при трансляции индекс считается не действительным (см `cmd_sentence`).
      vm->free = 1;
      vm->u[vm->free] = (struct rf_cell) { .next = vm->free + 1 };
   }
   return vm->u;
}

/**
 * Выделяет в памяти РЕФАЛ-машины ячейку для новых данных.
 * В случае, если индекс новой ячейки превышает допустимый, дальнейшее выделение
 * памяти не происходит, последняя ячейка адресует следующей 0-ю, а 0-я ячейка
 * закольцована сама на себя.
 * TODO должно быть адекватное решение.
 * \result индекс распределённой ячейки или 0.
 */
static inline
rf_index refal_vm_alloc_1(
      struct refal_vm   *vm)
{
   assert(vm);
   assert(vm->u);
   rf_index r = vm->free;
   assert(r);
   rf_index i = vm->u[r].next;
   // Достраиваем список, если следующее звено отсутствует.
   if (!vm->u[i].next) {
      vm->u[i].next = i + 1;
      vm->u[i].tag2 = 0;
      if (!vm->u[i].next) {
         return r;
      }
      if (i + 1 >= vm->size) {
         size_t size = vm->size * sizeof(rf_cell);
         void *p = refal_realloc(vm->u, size, 2 * size);
         if (!p) {
            vm->free = 0;
            return 0;
         }
         vm->u = p;
         vm->size *= 2;
      }
      vm->u[i + 1].tag = rf_undefined;
      vm->u[i + 1].prev = i;
   }
   vm->free = i;
   return r;
}

/**
 * Освобождает занятую РЕФАЛ-машиной память.
 */
static inline
void refal_vm_free(
      struct refal_vm   *vm)
{
   assert(vm);
   // TODO освободить ресурсы, ссылки на которые могут храниться в ячейках.
   refal_free(vm->u, vm->size);
   vm->u = 0;
   vm->size = 0;
   vm->free = 0;
}

/**
 * Проверяет состояние структуры.
 *
 * \result Ненулевой результат, если память распределена.
 */
static inline
void *refal_vm_check(
      const struct refal_vm   *vm,
      struct refal_message    *status)
{
   assert(vm);
   if (status && !vm->u) {
      critical_error(status, "недостаточно памяти для поля зрения", -errno, vm->size);
   }
   return vm->u;
}

static inline
void rf_vm_stats(
      const struct refal_vm   *vm,
      rf_index                prev,
      rf_index                next)
{
#ifndef  NDEBUG
   rf_index i = prev;
   rf_index view_count = 0;
   while (1) {
      assert(vm->u[vm->u[i].next].prev == i);
      i = vm->u[i].next;
      if (i == next)
         break;
      ++view_count;
   }
   rf_index forward_count = 0;
   i = vm->free;
   while (1) {
      ++forward_count;
      if (!vm->u[i].next)
         break;
      i = vm->u[i].next;
   }
   rf_index ununitialized = i + 1;
   rf_index backward_count = 0;
   while (1) {
      ++backward_count;
      if (i == vm->free)
         break;
      i = vm->u[i].prev;
   }
   printf("В поле зрения %u элементов. Активное подвыражение (%u %u). "
          "Освобождено: %u(%u). Не инициализировано: %u. Всего: %u\n",
          view_count, prev, next, forward_count, backward_count,
          vm->size - ununitialized, vm->size);
#endif
}

/**\}*/

/**\addtogroup auxiliary Вспомогательные функции.
 * \ingroup internal
 * \{
 *
 * Служат для трансформации поля зрения и управления свободными ячейками.
 *
 * Реализации функций принимают параметрами:
 * - адрес объекта с состоянием РЕФАЛ-машины;
 * - возможные дополнительные аргументы.
 *
 * Элемент поля зрения, адресуемый одним `rf_index` и не являющийся скобкой,
 * называется _s-переменной_.
 * _e-переменная_ — множество (возможно, пустое) ячеек, находящееся между парой
 * `prev` и `next`. Как правило, e-переменные ограничиваются s-переменными либо
 * скобками, индексы которых и используются в качестве границ.
 * Ограничение множества ссылками на внешние элементы позволяет добавлять и
 * удалять элементы на границах диапазона без сдвига `prev` и `next`.
 */

/**
 * Перемещает ячейки (prev ... next) в свободную часть списка.
 * Передаваемые аргументами границы используются в вызывающем коде для адресации
 * смежных областей, и, для упрощения реализации, требуют неизменности. Поэтому
 * переносятся после `vm->free` (что может показаться не всегда оптимальным).
 */
static inline
void rf_free_evar(
      struct refal_vm   *vm,
      rf_index          prev,
      rf_index          next)
{
   assert(prev != next);
   assert(vm->free != prev);
   const rf_index first = vm->u[prev].next;
   if (first != next) {
      const rf_index last  = vm->u[next].prev;
      // Связать ячейки, граничащие с удаляемыми.
      vm->u[prev].next = next;
      vm->u[next].prev = prev;
      // Добавить ячейки после vm->free.
      const rf_index heap = vm->u[vm->free].next;
      vm->u[vm->free].next = first;
      vm->u[vm->free].tag2 = 0;
      vm->u[first].prev = vm->free;
      vm->u[first].tag  = rf_undefined;
      vm->u[last].next = heap;
      vm->u[last].tag2 = 0;
      vm->u[heap].prev = last;
      vm->u[heap].tag  = rf_undefined;
      // TODO закрыть описатели (handle), при наличии.
   }
}

/**
 * Освобождает последнюю занятую ячейку.
 */
static inline
void rf_free_last(
      struct refal_vm   *vm)
{
   vm->free = vm->u[vm->free].prev;
}

/* * Эта и следующая функции унаследованы от А-Рефала и не применяются.
 *   TODO удалить либо исправить.
 *
 * Перемещает в поле зрения между prev и prev.next ячейки свободной части списка
 * начиная с first по [vm->free].prev включительно:

        rf_index two_ints = rf_alloc_int(vm, 0); // возвращает исходное vm->free
        rf_alloc_int(vm, 1);
        rf_insert_next(vm, prev_position, two_ints);
 */
static inline
void rf_insert_next(
      struct refal_vm   *vm,
      rf_index          prev,
      rf_index          first)
{
   assert(first != prev);
   assert(first != vm->free);
   const rf_index last = vm->u[vm->free].prev;
   const rf_index next = vm->u[prev].next;
   vm->u[last].next = next;
   vm->u[next].prev = last;
   vm->u[prev].next = first;
   vm->u[first].prev = prev;
}

/* *
 * Перемещает в поле зрения между next.prev и next ячейки свободной части списка
 * начиная с first по [vm->free].prev включительно.
 */
static inline
void rf_insert_prev(
      struct refal_vm   *vm,
      rf_index          next,
      rf_index          first)
{
   assert(first != next);
   const rf_index last = vm->u[vm->free].prev;
   const rf_index prev = vm->u[next].prev;
   vm->u[last].next = next;
   vm->u[next].prev = last;
   vm->u[prev].next = first;
   vm->u[first].prev = prev;
}

/**
 * Перемещает диапазон в новое место.
 */
static inline
void rf_splice_evar_prev(
      struct refal_vm   *vm,
      rf_index          prev,
      rf_index          next,
      rf_index          pos)
{
   const rf_index first = vm->u[prev].next;
   if (first != next) {
      const rf_index last  = vm->u[next].prev;
      const rf_index n_prev = vm->u[pos].prev;
      vm->u[prev].next = next;
      vm->u[next].prev = prev;
      vm->u[pos].prev  = last;
      vm->u[last].next = pos;
      vm->u[n_prev].next = first;
      vm->u[first].prev = n_prev;
   }
}

/**
 * Добавляет в свободную часть списка содержимое диапазона, удаляя исходное.
 * Иначе говоря, перемещает e-переменную в пространство формирования результата.
 * Передаваемые аргументами границы используются в вызывающем коде для адресации
 * смежных областей, и, для упрощения реализации, требуют неизменности. Поэтому
 * не должны совпадать с `vm->free`.
 *
 * `vm->free` не изменяется.
 * Вызывающая сторона контролирует, что диапазон не пуст.
 *
 * \result номер начальной ячейки.
 */
static inline
rf_index rf_alloc_evar_move(
      struct refal_vm   *vm,
      rf_index          prev,
      rf_index          next)
{
   assert(prev != next);
   assert(vm->free != prev);
   assert(vm->free != next);
   const rf_index first = vm->u[prev].next;
   assert(first != next);
   const rf_index last = vm->u[next].prev;
   // Связать ячейки, граничащие с удаляемыми.
   vm->u[prev].next = next;
   vm->u[next].prev = prev;
   // Вставляем перед свободными.
   const rf_index allocated = vm->u[vm->free].prev;
   vm->u[allocated].next = first;
   vm->u[first].prev = allocated;
   vm->u[vm->free].prev = last;
   vm->u[last].next = vm->free;
   return first;
}

/**
 * Добавляет в свободную часть списка значение и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_value(
      struct refal_vm   *vm,
      uint64_t          value,
      rf_type           tag)
{
   rf_index i = refal_vm_alloc_1(vm);
   vm->u[i].data = value;
   vm->u[i].tag  = tag;
   return i;
}

/**
 * Добавляет в свободную часть списка команду интерпретатора и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_command(
      struct refal_vm   *vm,
      rf_type           tag)
{
   return rf_alloc_value(vm, 0, tag);
}

/**
 * Добавляет в свободную часть списка символ идентификатора и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_atom(
      struct refal_vm   *vm,
      const char        *str)
{
   rf_index i = refal_vm_alloc_1(vm);
   vm->u[i].atom = str;
   vm->u[i].tag  = rf_atom;
   return i;
}

/**
 * Добавляет в свободную часть списка символ и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_char(
      struct refal_vm   *vm,
      wchar_t           chr)
{
   // Очищаем старшие разряды, что бы работало обобщённое сравнение
   // s-переменных.
   return rf_alloc_value(vm, chr, rf_char);
}

/**
 * Добавляет в свободную часть списка целое число и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_int(
      struct refal_vm   *vm,
      rf_int            num)
{
   return rf_alloc_value(vm, num, rf_number);
}

/**
 * Добавляет в свободную часть списка символ, получаемый из последовательности
 * UTF-8. Если текущий октет представляет часть последовательности, происходит
 * частичное декодирование символа, дополняемое при следующем вызове.
 * \retval state указывает, сколько октетов требуется для полного декодирования
 *         текущего символа.
 * \result Номер содержащей символ ячейки.
 */
static inline
rf_index rf_alloc_char_decode_utf8(
      struct refal_vm   *vm,
      unsigned char     octet,   ///< текущий элемент последовательности UTF-8.
      unsigned          *state)  ///< состояние декодера. 0 при первом вызове.
{
   rf_index i;
   switch (*state) {
   case 0:
      i = refal_vm_alloc_1(vm);
      vm->u[i].tag  = rf_char;
      switch (octet) {
      // ASCII
      case 0x00 ... 0x7f:
         vm->u[i].data = octet;
         return i;
      // 2 байта на символ.
      case 0xc0 ... 0xdf:
         *state = 1;
         vm->u[i].data = 0x1f & octet;
         return i;
      // 3 байта на символ.
      case 0xe0 ... 0xef:
         *state = 2;
         vm->u[i].data = 0x0f & octet;
         return i;
      // 4 байта на символ.
      case 0xf0 ... 0xf4:
         *state = 3;
         vm->u[i].data = 0x03 & octet;
         return i;
      default:
         assert(0);
         vm->u[i].data = 0;
         return i;
      }
   default:
      assert(0);
   case 1:
   case 2:
   case 3:
      i = vm->u[vm->free].prev;
      assert(vm->u[i].tag == rf_char);
      --*state;
      vm->u[i].chr = (vm->u[i].chr << 6) | (0x3f & octet);
      return i;
   }
}

/**
 * Кодирует символ из ячейки РЕФАЛ-машины в последовательность UTF-8.
 * \result количество байт последовательности.
 */
static inline
unsigned rf_encode_utf8(
      const struct refal_vm   *vm,
      rf_index                s,
      char                    ptr[4])
{
   wchar_t chr = vm->u[s].chr;
   unsigned size = 0;
   if (chr < 0x80) {
      ptr[0] = chr;
      size = 1;
   } else if (chr < 0x800) {
      ptr[0] = 0xc0 | (chr >> 6);
      ptr[1] = 0x80 | (chr & 0x3f);
      size = 2;
   } else if (chr < 0x10000) {
      ptr[0] = 0xe0 | (chr >> 12);
      ptr[1] = 0x80 | ((chr >> 6) & 0x3f);
      ptr[2] = 0x80 | (chr & 0x3f);
      size = 3;
   } else {
      ptr[0] = 0xf0 | ( chr >> 18);
      ptr[1] = 0x80 | ((chr >> 12) & 0x3f);
      ptr[2] = 0x80 | ((chr >>  6) & 0x3f);
      ptr[3] = 0x80 | (chr & 0x3f);
      size = 4;
   }
   return size;
}

/**
 * Размещает в свободной памяти строку.
 * \result Номер содержащей первый символ строки ячейки.
 */
static inline
rf_index rf_alloc_string(
      struct refal_vm   *vm,
      const char        *str)
{
   rf_index r = vm->free;
   unsigned state = 0;
   char c;
   while ((c = *str++)) {
      rf_alloc_char_decode_utf8(vm, (unsigned char)c, &state);
   }
   return r;
}

/**
 * Связывает открывающую и закрывающую скобки ссылками друг на друга.
 */
static inline
void rf_link_brackets(
      struct refal_vm   *vm,
      rf_index          opening,
      rf_index          closing)
{
   assert(vm->u[opening].tag == rf_opening_bracket);
   assert(vm->u[closing].tag == rf_closing_bracket);
   vm->u[opening].link = closing;
   vm->u[closing].link = opening;
}

/**
 * Размещает в свободной памяти массив строк,
 * заключая каждую в структурные скобки.
 */
static inline
rf_index rf_alloc_strv(
      struct refal_vm   *vm,
      int               strc,
      const char *const *strv)
{
   rf_index r = vm->free;
   for (int i = 0; i < strc; ++i) {
      rf_index ob = rf_alloc_command(vm, rf_opening_bracket);
      rf_alloc_string(vm, strv[i]);
      rf_link_brackets(vm, ob, rf_alloc_command(vm, rf_closing_bracket));
   }
   return r;
}

/**
 * Проверяет, пусто ли подвыражение (e-переменная).
 * \result ненулевое значение, если между prev и next отсутствуют звенья.
 */
static inline
int rf_is_evar_empty(
      struct refal_vm   *vm,
      rf_index          prev,
      rf_index          next)
{
    return vm->u[prev].next == next;
}

/**
 * Сравнивает s-переменные.
 */
static inline
int rf_svar_equal(
      struct refal_vm   *vm,
      rf_index          s1,
      rf_index          s2)
{
   assert(s1 != s2);
   // Сначала сравниваем данные, поскольку:
   // 1. даже при равенстве тегов, они вероятно, различаются;
   // 2. теги хранятся в битовом поле и требуют команды AND для выделения.
   return vm->u[s1].data == vm->u[s2].data
       && vm->u[s1].tag  == vm->u[s2].tag;
}

/**\}*/

/**\ingroup library
 *
 * Прототип функции, не изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 * \result
 *       - 0   — выполнено успешно;
 *       - > 0 — неподходящее поле зрения (отождествление невозможно);
 *       - < 0 — ошибка среды исполнения (при вызове функций ОС).
 */
typedef int rf_cfunction(const struct refal_vm *vm, rf_index prev, rf_index next);

/**\ingroup library
 *
 * Прототип функции, изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 * \result
 *       - 0   — выполнено успешно;
 *       - > 0 — неподходящее поле зрения (отождествление невозможно);
 *       - < 0 — ошибка среды исполнения (при вызове функций ОС).
 */
typedef int rf_function(struct refal_vm *vm, rf_index prev, rf_index next);

/**
 * Связывает имя функции (текстовое) с её реализацией.
 */
struct refal_import_descriptor {
   const char  *name;
   // Вызывающая сторона считает, что функция меняет состояние РЕФАЛ-машины.
   // Вариант с константностью добавлен для наглядности определений
   // и что бы избежать приведений типа.
   union {
      rf_function    *function;
      rf_cfunction   *cfunction;
   };
};

