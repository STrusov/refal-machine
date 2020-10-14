/**\file
 * Реализация РЕФАЛ интерпретатора.
 */

#include "library.h"
#include "rtrie.h"
#include "refal.h"


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
      struct refal_trie    *rt,
      const char           *name,
      struct refal_message *st)
{
   st->source = name;

   size_t source_size = 0;
   const char *source = mmap_file(name, &source_size);
   assert(source != MAP_FAILED);

   return 0;
}


int main(int argc, char **argv)
{
   struct refal_message status = {
         .handler = refal_message_print,
         .source  = argv[0],
   };

   struct refal_trie rtrie = rtrie_alloc(15);
   if (rtrie_check(&rtrie, &status)) {

      refal_import(&rtrie, library, &status);

      struct rtrie_val val;
      val = rtrie_get_value(&rtrie, "Prout");
      val = rtrie_get_value(&rtrie, "Print");

      process_file(&rtrie, "tests/simple_hello.ref", &status);

   }
   return 0;
}
