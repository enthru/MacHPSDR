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
  if(strcmp(i18n_tr("TX MONITOR"),"МОНИТОР TX")!=0) return 16;
  if(strcmp(i18n_tr("MIC & DRIVE"),"МИКРОФОН И МОЩНОСТЬ")!=0) return 17;
  if(strcmp(i18n_tr("Microphone Level"),"Уровень микрофона")!=0) return 18;
  if(strcmp(i18n_tr("Microphone Gain"),"Усиление микрофона")!=0) return 19;
  if(strcmp(i18n_tr("Drive"),"Мощность TX")!=0) return 20;
  if(strcmp(i18n_tr("Power out"),"Выходная мощность")!=0) return 23;
  if(strcmp(i18n_tr("Device rate:"),"Частота устройства:")!=0) return 24;
  if(strcmp(i18n_tr("Freetune:"),"Freetune:")!=0) return 25;
  if(strcmp(i18n_tr("Gain"),"Усиление")!=0) return 28;
  if(strcmp(i18n_tr("<b>Gain</b>"),"<b>Усиление</b>")!=0) return 38;
  if(strcmp(i18n_tr("ASSIGNED TX"),"НАЗНАЧЕН TX")!=0) return 37;
  if(strcmp(i18n_tr("Tuning"),"Настройка")!=0) return 29;
  if(strcmp(i18n_tr("Zoom in"),"Увеличить масштаб")!=0) return 30;
  if(strcmp(i18n_tr("Event"),"Событие")!=0) return 31;
  if(strcmp(i18n_tr("Bundled components"),"Встроенные компоненты")!=0) return 32;
  if(strcmp(i18n_tr("Narrow-band, 10489.500–10490.000 MHz (SSB/CW/digi)"),
            "Узкополосный, 10489,500–10490,000 MHz (SSB/CW/digi)")!=0)
    return 36;
  if(strcmp(i18n_tr("This device does not support diversity reception (it needs Protocol 1 or Protocol 2, two receivers and two ADCs), so it cannot be enabled here."),
            "Это устройство не поддерживает разнесённый приём: нужны Protocol 1 или Protocol 2, два приёмника и два ADC. Поэтому включить его здесь нельзя.")!=0)
    return 33;
  if(strcmp(i18n_tr("Active device rate: %s. Maximum receiver span: %s."),
            "Активная частота устройства: %s. Максимальная полоса приёмника: %s.")!=0)
    return 26;
  char rate_status[256];
  g_snprintf(rate_status,sizeof(rate_status),
             i18n_tr("Active device rate: %s. Maximum receiver span: %s."),
             "768k","384k");
  if(strcmp(rate_status,
            "Активная частота устройства: 768k. Максимальная полоса приёмника: 384k.")!=0)
    return 27;
  if(strcmp(i18n_tr("VFO"),"VFO")!=0) return 3; /* technical term */
  if(strcmp(i18n_tr("AGC"),"AGC")!=0) return 13; /* technical term */

  /* Re-initialization proves that the independent global preference survives
   * without relying on any particular radio's .props file. */
  i18n_init(dir);
  if(i18n_language()!=I18N_RU) return 4;
  i18n_set_language(I18N_UK);
  if(strcmp(i18n_tr("Save"),"Зберегти")!=0) return 5;
  if(strcmp(i18n_tr("Close this receiver"),"Закрити цей приймач")!=0) return 14;
  if(strcmp(i18n_tr("Microphone Gain"),"Підсилення мікрофона")!=0) return 21;
  if(strcmp(i18n_tr("Transmit"),"Передавання")!=0) return 34;
  i18n_set_language(I18N_BE);
  if(strcmp(i18n_tr("No settings found"),"Налады не знойдзены")!=0) return 6;
  if(strcmp(i18n_tr("MIDI Device"),"Прылада MIDI")!=0) return 15;
  if(strcmp(i18n_tr("Drive"),"Магутнасць TX")!=0) return 22;
  if(strcmp(i18n_tr("Filter"),"Фільтр")!=0) return 35;
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
