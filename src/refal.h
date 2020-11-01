/**\defgroup refal Язык РЕФАЛ.
 * ## О языке. ##
 *
 * Существует три общепринятых варианта формализации алгоритмов:
 * - Машина Тьюринга (реализуется императивными языками);
 * - Стрелка Чёрча (реализуется функциональными языками);
 * - Нормальный алгорифм Маркова (НАМ).
 *
 * Язык РЕкурсивных Формул АЛгоритмический натуральным образом реализует НАМ.
 *
 * \file
 * \brief Основной интерфейс РЕФАЛ-машины.
 *
 * Описание структур данных и имплементация вспомогательных функций,
 * в совокупности образующих РЕФАЛ-машину, способную взаимодействовать
 * с реализованными на языках C и C++ (под)программами.
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
   rf_undefined,        ///< Пустая ячейка (не инициализирована).
   rf_char,             ///< Символ (одна буква в кодировке UTF-8).
   rf_number,           ///< Целое число.
   rf_atom,             ///< Идентификатор (рассматривается целиком).
   rf_opening_bracket,  ///< Открывающая скобка.
   rf_closing_bracket,  ///< Закрывающая скобка.
   // Команды интерпретатора.
   rf_sentence,         ///< Начало предложения. Содержит ссылку на следующее.
   rf_equal,            ///< Разделяет выражение-образец от общего выражения.
   rf_execute,          ///< Открывающая вычислительная скобка <
   rf_execute_close,    ///< Закрывающая вычислительная скобка >
   rf_identifier,       ///< Идентификатор (функция) // TODO rf_atom
   rf_svar,             ///< s-переменная (односимвольная).
   rf_tvar,             ///< t-переменная (s- либо выражение в скобках).
   rf_evar,             ///< e-переменная (произвольное количество элементов.
   rf_complete,         ///< Общее выражение завершено.
   _rf_types_count
} rf_type;

static_assert(_rf_types_count <= 1<<4, "Значение хранится в 4-х разрядах.");

/**
 * Адресует ячейки памяти РЕФАЛ-машины.
 */
typedef uint32_t rf_index;

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
   rf_index    tag2:4;     ///< Резерв (не используется).
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
 * частью подвыраждения (операция вставки).
 */
typedef struct refal_vm {
   rf_cell     *cell;   ///< Массив, содержащий ячейки.
   rf_index    size;    ///< Размер массива.
   rf_index    free;    ///< Первый свободный элемент.
} rf_vm;

/**
 * Резервирует память для хранения поля зрения РЕФАЛ программы.
 * Связывает ячейки массива в список.
 * \result Ненулевое значение в случае успеха.
 */
static inline
void *refal_vm_init(
      struct refal_vm   *restrict vm,  ///< Структура для инициализации.
      rf_index size)                   ///< Предполагаемый размер (в ячейках).
{
   vm->cell = malloc(size * sizeof(rf_cell));
   if (vm->cell) {
      vm->size = size;
      for (rf_index i = 0; i < size; ++i) {
         vm->cell[i].tag = rf_undefined;
         vm->cell[i].next = i + 1;
         vm->cell[i].tag2 = 0;
         vm->cell[i + 1].prev = i;
      }
      vm->cell[size - 1].next = 0;
      vm->free = 2;   // TODO 1? \see `rf_insert_next()`
   }
   return vm->cell;
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
   free(vm->cell);
   vm->cell = 0;
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
      struct refal_vm      *vm,
      struct refal_message *status)
{
   assert(vm);
   if (status && !vm->cell) {
      critical_error(status, "недостаточно памяти для поля зрения", -errno, vm->size);
   }
   return vm->cell;
}

static inline
void rf_vm_stats(
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          next)
{
#ifndef  NDEBUG
   rf_index i = prev;
   size_t view_count = 0;
   while (1) {
      assert(vm->cell[vm->cell[i].next].prev == i);
      i = vm->cell[i].next;
      if (i == next)
         break;
      ++view_count;
   }
   size_t forward_count = 0;
   i = vm->free;
   while (1) {
      ++forward_count;
      if (!vm->cell[i].next)
         break;
      i = vm->cell[i].next;
   }
   size_t backward_count = 0;
   while (1) {
      ++backward_count;
      if (i == vm->free)
         break;
      i = vm->cell[i].prev;
   }
   printf("В поле зрения %lu элементов. Активное подвыражение (%u %u). "
          "Свободно: %lu(%lu).\n", view_count, prev, next,
          forward_count, backward_count);
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
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          next)
{
   assert(prev != next);
   assert(vm->free != prev);
   const rf_index first = vm->cell[prev].next;
   if (first != next) {
      const rf_index last  = vm->cell[next].prev;
      // Связать ячейки, граничащие с удаляемыми.
      vm->cell[prev].next = next;
      vm->cell[next].prev = prev;
      // Добавить ячейки после vm->free.
      const rf_index heap = vm->cell[vm->free].next;
      vm->cell[vm->free].next = first;
      vm->cell[vm->free].tag2 = 0;
      vm->cell[first].prev = vm->free;
      vm->cell[first].tag  = rf_undefined;
      vm->cell[last].next = heap;
      vm->cell[last].tag2 = 0;
      vm->cell[heap].prev = last;
      vm->cell[heap].tag  = rf_undefined;
      // TODO закрыть описатели (handle), при наличии.
   }
}

/**
 * Освобождает последнюю занятую ячейку.
 */
static inline
void rf_free_last(
      struct refal_vm   *restrict vm)
{
   vm->free = vm->cell[vm->free].prev;
}

/**
 * Перемещает в поле зрения между prev и prev.next ячейки свободной части списка
 * начиная с first по [vm->free].prev включительно:

        rf_index two_ints = rf_alloc_int(vm, 0); // возвращает исходное vm->free
        rf_alloc_int(vm, 1);
        rf_insert_next(vm, prev_position, two_ints);
 */
static inline
void rf_insert_next(
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          first)
{
   assert(first != prev);
   assert(first != vm->free);
   const rf_index last = vm->cell[vm->free].prev;
   const rf_index next = vm->cell[prev].next;
   vm->cell[last].next = next;
   vm->cell[next].prev = last;
   vm->cell[prev].next = first;
   vm->cell[first].prev = prev;
}

/**
 * Перемещает в поле зрения между next.prev и next ячейки свободной части списка
 * начиная с first по [vm->free].prev включительно.
 */
static inline
void rf_insert_prev(
      struct refal_vm   *restrict vm,
      rf_index          next,
      rf_index          first)
{
   assert(first != next);
   const rf_index last = vm->cell[vm->free].prev;
   const rf_index prev = vm->cell[next].prev;
   vm->cell[last].next = next;
   vm->cell[next].prev = last;
   vm->cell[prev].next = first;
   vm->cell[first].prev = prev;
}

/**
 * Перемещает диапазон в новое место.
 */
static inline
void rf_splice_evar_prev(
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          next,
      rf_index          pos)
{
   const rf_index first = vm->cell[prev].next;
   if (first != next) {
      const rf_index last  = vm->cell[next].prev;
      const rf_index n_prev = vm->cell[pos].prev;
      vm->cell[prev].next = next;
      vm->cell[next].prev = prev;
      vm->cell[pos].prev  = last;
      vm->cell[last].next = pos;
      vm->cell[n_prev].next = first;
      vm->cell[first].prev = n_prev;
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
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          next)
{
   assert(prev != next);
   assert(vm->free != prev);
   assert(vm->free != next);
   const rf_index first = vm->cell[prev].next;
   assert(first != next);
   const rf_index last = vm->cell[next].prev;
   // Связать ячейки, граничащие с удаляемыми.
   vm->cell[prev].next = next;
   vm->cell[next].prev = prev;
   // Вставляем перед свободными.
   const rf_index allocated = vm->cell[vm->free].prev;
   vm->cell[allocated].next = first;
   vm->cell[first].prev = allocated;
   vm->cell[vm->free].prev = last;
   vm->cell[last].next = vm->free;
   return first;
}

/**
 * Добавляет в свободную часть списка значение и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_value(
      struct refal_vm   *restrict vm,
      uint64_t          value,
      rf_type           tag)
{
   assert(vm->cell);
   assert(vm->free);
   rf_index i = vm->free;
   vm->cell[i].data = value;
   vm->cell[i].tag  = tag;
   vm->free = vm->cell[i].next;
   return i;
}

/**
 * Добавляет в свободную часть списка команду интерпретатора и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_command(
      struct refal_vm   *restrict vm,
      rf_type           tag)
{
   return rf_alloc_value(vm, 0, tag);
}

/**
 * Добавляет в свободную часть списка символ идентификатора и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_atom(
      struct refal_vm   *restrict vm,
      const char        *str)
{
   assert(vm->cell);
   assert(vm->free);
   rf_index i = vm->free;
   vm->cell[i].atom = str;
   vm->cell[i].tag  = rf_atom;
   vm->free = vm->cell[i].next;
   return i;
}

/**
 * Добавляет в свободную часть списка символ и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_char(
      struct refal_vm   *restrict vm,
      wchar_t           chr)
{
    // Обнуляем старшие разряды, что бы работало обобщённое сравнение
    // s-переменных.
    return rf_alloc_value(vm, chr, rf_char);
}

/**
 * Добавляет в свободную часть списка целое число и возвращает номер ячейки.
 */
static inline
rf_index rf_alloc_int(
      struct refal_vm   *restrict vm,
      rf_int            num)
{
   return rf_alloc_value(vm, num, rf_number);
}

/**
 * Проверяет, пусто ли подвыражение (e-переменная).
 * \result ненулевое значение, если между prev и next отсутствуют звенья.
 */
static inline
int rf_is_evar_empty(
      struct refal_vm   *restrict vm,
      rf_index          prev,
      rf_index          next)
{
    return vm->cell[prev].next == next;
}

/**
 * Сравнивает s-переменные.
 */
static inline
int rf_svar_equal(
      struct refal_vm   *restrict vm,
      rf_index          s1,
      rf_index          s2)
{
   assert(s1 != s2);
   /* Сначала сравниваем данные, поскольку:                                */
   /* 1. даже при равенстве тегов, они вероятно, различаются;              */
   /* 2. теги хранятся в битовом поле и требуют команды AND для выделения. */
   return vm->cell[s1].data == vm->cell[s2].data
       && vm->cell[s1].tag  == vm->cell[s2].tag;
}

/**\}*/

/**\ingroup library
 *
 * Прототип функции, не изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 */
typedef void rf_cfunction(const rf_vm *restrict vm, rf_index prev, rf_index next);

/**\ingroup library
 *
 * Прототип функции, изменяющей состояние РЕФАЛ-машины.
 * \param vm   Указатель на объект виртуальной машины.
 * \param prev Элемент перед первым поля зрения функции.
 * \param next Элемент после последнего поля зрения функции.
 */
typedef void rf_function(rf_vm *restrict vm, rf_index prev, rf_index next);

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


/**\}*/
