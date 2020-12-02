/**\file
 * \brief Интерфейс интерпретатора.
 *
 * \addtogroup interpreter Интерпретатор байт-кода РЕФАЛ программы.
 * \{
 */

#pragma once

#include "refal.h"

/**
 * Конфигурация интерпретатора.
 * Размеры в байтах, должны быть кратны размеру страницы.
 * Рост стека переменных косвенно ограничен стеком вызова,
 * отдельный лимит не задан (TODO надо ли?).
 */
struct refal_interpreter_config {
   unsigned call_stack_size;     ///< Начальный размер стека вызовов.
   unsigned call_stack_max;      ///< Максимальный размер стека вызовов.
   unsigned var_stack_size;      ///< Начальный размер стека переменных.
   unsigned brackets_stack_size; ///< Начальный размер стека структурных скобок.
};

/**
 * Исполнение байт-кода РЕФАЛ-машины.
 * Поле зрения располагается _между_ prev и next.
 * \result
 *         - Отрицательное значение при ошибке исполнения.
 *         - 0 — Успех (Поле Зрения может быть не пусто).
 *         - Положительное значение — Отождествление Невозможно.
 */
int refal_interpret_bytecode(
      struct refal_interpreter_config  *cfg, ///< Конфигурация интерпретатора.
      struct refal_vm      *vm,        ///< Память Рефал-машины.
      rf_index             prev,       ///< Левая граница поля зрения.
      rf_index             next,       ///< Правая граница поля зрения.
      rf_index             sentence,   ///< Начальная инструкция.
      struct refal_message *st
      );

/**\}*/
