/**\file
 * \brief Исполнитель РЕФАЛ-программ.
 */

#include "library.h"
#include "interpreter.h"
#include "translator.h"


int main(int argc, char **argv)
{
   struct refal_message status = {
         .handler = refal_message_print,
         .source  = "Интерпретатор РЕФАЛ",
   };

   if (argc != 2) {
      critical_error(&status, "укажите одно имя файла с исходным текстом", argc, 0);
      return 0;
   }

   // Память РЕФАЛ-машины (байт-код и поле зрения совмещены).
   struct refal_vm   vm;
   refal_vm_init(&vm, 500);
   if (refal_vm_check(&vm, &status)) {

      // Таблица символов.
      struct refal_trie ids;
      rtrie_alloc(&ids, 100);
      if (rtrie_check(&ids, &status)) {

         refal_import(&ids, library);
         refal_translate_file_to_bytecode(&ids, &vm, argv[1], &status);

         struct rtrie_val entry = rtrie_get_value(&ids, "Go");
         if (entry.tag != rft_byte_code)
            entry = rtrie_get_value(&ids, "go");

//         rtrie_free(&ids);

         if (entry.tag != rft_byte_code) {
            critical_error(&status, "не определена начальная функция (go)", entry.value, 0);
         } else {
            // Границы поля зрения:
            rf_index next = vm.free;
            rf_index prev = vm.u[next].prev;
            refal_interpret_bytecode(&vm, prev, next, entry.value, &status);
         }
      }
      rtrie_free(&ids);
   }
   refal_vm_free(&vm);

   return 0;
}
