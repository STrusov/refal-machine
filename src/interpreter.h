/**\file
 * \brief Интерфейс интерпретатора.
 *
 * \addtogroup interpreter Интерпретатор байт-кода РЕФАЛ программы.
 * \{
 */

#pragma once

#include "refal.h"

/**
 * Исполнение байт-кода РЕФАЛ-машины.
 * Поле зрения располагается _между_ prev и next.
 */
int refal_interpret_bytecode(
      struct refal_vm      *vm,
      rf_index             prev,       ///< Левая граница поля зрения.
      rf_index             next,       ///< Правая граница поля зрения.
      rf_index             sentence,   ///< Начальная инструкция.
      struct refal_message *st
      );

/**\}*/
