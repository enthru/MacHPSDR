/* Headless smoke test for language selection, catalogue lookup and persistence. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"

int main(void) {
  GError *error=NULL;
  char *dir=g_dir_make_tmp("machpsdr-i18n-XXXXXX",&error);
  if(dir==NULL) {
    fprintf(stderr,"i18n: cannot create temp dir: %s\n",error->message);
    g_error_free(error);
    return 1;
  }

  i18n_init(dir);
  i18n_set_language(I18N_RU);
  if(strcmp(i18n_tr("Start Radio"),"Запустить радио")!=0) return 2;
  if(strcmp(i18n_tr("VFO"),"VFO")!=0) return 3; /* technical term */

  /* Re-initialization proves that the independent global preference survives
   * without relying on any particular radio's .props file. */
  i18n_init(dir);
  if(i18n_language()!=I18N_RU) return 4;
  i18n_set_language(I18N_UK);
  if(strcmp(i18n_tr("Save"),"Зберегти")!=0) return 5;
  i18n_set_language(I18N_BE);
  if(strcmp(i18n_tr("No settings found"),"Налады не знойдзены")!=0) return 6;
  i18n_set_language(I18N_EN);
  if(strcmp(i18n_tr("Save"),"Save")!=0) return 7;

  char *path=g_build_filename(dir,"language",NULL);
  g_remove(path);
  g_rmdir(dir);
  g_free(path);
  g_free(dir);
  puts("i18n: all checks passed");
  return 0;
}
