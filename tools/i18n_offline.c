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
  if(strcmp(i18n_tr("Configure"),"Настройки")!=0) return 8;
  if(strcmp(i18n_tr("Calibrate TX"),"Калибровка TX")!=0) return 9;
  if(strcmp(i18n_tr("Hide FT8 Panel"),"Скрыть панель FT8")!=0) return 10;
  if(strcmp(i18n_tr("Open the settings window"),"Открыть окно настроек")!=0) return 11;
  if(strcmp(i18n_tr("AGC attack:"),"Атака AGC:")!=0) return 12;
  if(strcmp(i18n_tr("VFO"),"VFO")!=0) return 3; /* technical term */
  if(strcmp(i18n_tr("AGC"),"AGC")!=0) return 13; /* technical term */

  /* Re-initialization proves that the independent global preference survives
   * without relying on any particular radio's .props file. */
  i18n_init(dir);
  if(i18n_language()!=I18N_RU) return 4;
  i18n_set_language(I18N_UK);
  if(strcmp(i18n_tr("Save"),"Зберегти")!=0) return 5;
  if(strcmp(i18n_tr("Close this receiver"),"Закрити цей приймач")!=0) return 14;
  i18n_set_language(I18N_BE);
  if(strcmp(i18n_tr("No settings found"),"Налады не знойдзены")!=0) return 6;
  if(strcmp(i18n_tr("MIDI Device"),"Прылада MIDI")!=0) return 15;
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
