/**\file
 * \brief Интерфейс исполнителя (интерпретатора).
 *
 * \addtogroup interpreter Исполнитель опкодов РЕФАЛ программы.
 * \{
 */

#pragma once

#include "refal.h"

#ifndef REFAL_INTERPRETER_BOXED_PATTERNS
#define REFAL_INTERPRETER_BOXED_PATTERNS     64
#endif

/**
 * Конфигурация исполнителя.
 * Размеры изменяемых стеков - в байтах, должны быть кратны размеру страницы.
 * Рост стека переменных косвенно ограничен стеком вызова,
 * отдельный лимит не задан (TODO надо ли?).
 */
struct refal_interpreter_config {
   unsigned call_stack_size;     ///< Начальный размер стека вызовов.
   unsigned call_stack_max;      ///< Максимальный размер стека вызовов.
   unsigned var_stack_size;      ///< Начальный размер стека переменных.
   unsigned brackets_stack_size; ///< Начальный размер стека структурных скобок.

   /// Количество уровней вложенности образцов в ящиках.
   /// Используется при просмотре предложения-образца и не участвует в
   /// рекурсивных вызовах результатного, потому большой стек вряд ли потребуется.
   unsigned boxed_patterns;

   /// Допустимое количество переменных в предложении (определяется транслятором).
   unsigned locals;
};

/**
 * Исполнение опкодов РЕФАЛ-машины.
 * Поле зрения располагается _между_ prev и next.
 * \result
 *         - Отрицательное значение при ошибке исполнения.
 *         - 0 — Успех (Поле Зрения может быть не пусто).
 *         - Положительное значение — Отождествление Невозможно.
 */
int refal_run_opcodes(
      struct refal_interpreter_config  *cfg, ///< Конфигурация исполнителя.
      struct refal_vm      *vm,        ///< Память Рефал-машины.
      rf_index             prev,       ///< Левая граница поля зрения.
      rf_index             next,       ///< Правая граница поля зрения.
      rf_index             sentence,   ///< Начальная инструкция.
      struct refal_message *st
      );

/**\}*/
