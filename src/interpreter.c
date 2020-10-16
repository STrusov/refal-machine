/**\file
 * \brief Реализация РЕФАЛ интерпретатора.
 */

#include <error.h>
#include "library.h"
#include "rtrie.h"
#include "refal.h"

size_t refal_parse_text(
      struct refal_trie    *ids,
      struct refal_vm      *vm,
      const char           *begin,
      const char           *end,
      struct refal_message *st
      );


struct refal_interpreter {
   struct refal_trie ids;  ///< Идентификаторы.
   struct refal_vm   vm;   ///< Байт-код и поле зрения.
};

static inline
void refal_interpreter_init(
      struct refal_interpreter   *it)
{
   it->ids = rtrie_alloc(25);
   it->vm  = refal_vm_init(100);
}

static inline
rtrie_index refal_import(
      struct refal_trie                      *rt,
      const struct refal_import_descriptor   *lib,
      struct refal_message                   *ectx)
{
   const rtrie_index free = rt->free;
   for (int ordinal = 0; lib->name; ++lib, ++ordinal) {
      const char *p = lib->name;
      assert(p);
      rtrie_index idx = rtrie_insert_first(rt, *p++);
      while (*p) {
         idx = rtrie_insert_next(rt, idx, *p++);
      }
      rt->n[idx].val.tag   = rf_machine_code;
      rt->n[idx].val.value = ordinal;
   }
   return rt->free - free;
}


static
int process_file(
      struct refal_interpreter   *ri,
      const char           *name,
      struct refal_message *st)
{
   st->source = name;

   size_t source_size = 0;
   const char *source = mmap_file(name, &source_size);
   if (source == MAP_FAILED) {
      if (st)
         critical_error(st, "исходный текст недоступен", -errno, source_size);
   } else {
      refal_parse_text(&ri->ids, &ri->vm, source, &source[source_size], st);

   }

   return 0;
}


int main(int argc, char **argv)
{
   struct refal_message status = {
         .handler = refal_message_print,
         .source  = argv[0],
   };

   struct refal_interpreter refint;
   refal_interpreter_init(&refint);

   if (rtrie_check(&refint.ids, &status) && refal_vm_check(&refint.vm, &status)) {

      refal_import(&refint.ids, library, &status);

      struct rtrie_val val;
      val = rtrie_get_value(&refint.ids, "Prout");
      assert(val.tag);
      val = rtrie_get_value(&refint.ids, "Print");
      assert(val.tag);

      process_file(&refint, "tests/simple_hello.ref", &status);

   }
   return 0;
}
