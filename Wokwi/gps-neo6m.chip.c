#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UM_SEGUNDO 1000000

typedef struct {
  uart_dev_t uart;
  uint32_t atributo_sinal;
  uint32_t segundos;
} estado_t;

static void enviar_nmea(estado_t *chip, const char *corpo) {
  uint8_t soma = 0;
  for (const char *p = corpo; *p; p++) {
    soma ^= (uint8_t)*p;
  }
  char linha[128];
  int tamanho = snprintf(linha, sizeof(linha), "$%s*%02X\r\n", corpo, soma);
  uart_write(chip->uart, (uint8_t *)linha, tamanho);
}

static void ao_timer(void *dados) {
  estado_t *chip = (estado_t *)dados;
  uint32_t t = chip->segundos++;
  int hora = (14 + t / 3600) % 24;
  int minuto = (t / 60) % 60;
  int segundo = t % 60;
  int com_sinal = attr_read_float(chip->atributo_sinal) >= 0.5f;
  char corpo[110];

  if (com_sinal) {
    snprintf(corpo, sizeof(corpo),
             "GPGGA,%02d%02d%02d.00,2333.0312,S,04637.9986,W,1,08,1.0,760.0,M,0.0,M,,",
             hora, minuto, segundo);
    enviar_nmea(chip, corpo);
    snprintf(corpo, sizeof(corpo),
             "GPRMC,%02d%02d%02d.00,A,2333.0312,S,04637.9986,W,0.0,0.0,171126,,,A",
             hora, minuto, segundo);
    enviar_nmea(chip, corpo);
  } else {
    snprintf(corpo, sizeof(corpo), "GPGGA,%02d%02d%02d.00,,,,,0,00,99.9,,M,,M,,", hora, minuto, segundo);
    enviar_nmea(chip, corpo);
    snprintf(corpo, sizeof(corpo), "GPRMC,%02d%02d%02d.00,V,,,,,,,171126,,,N", hora, minuto, segundo);
    enviar_nmea(chip, corpo);
  }
}

void chip_init(void) {
  estado_t *chip = malloc(sizeof(estado_t));
  chip->segundos = 0;
  chip->atributo_sinal = attr_init_float("sinal", 1.0f);

  const uart_config_t config_uart = {
    .tx = pin_init("TX", INPUT_PULLUP),
    .rx = pin_init("RX", INPUT),
    .baud_rate = 9600,
    .user_data = chip,
  };
  chip->uart = uart_init(&config_uart);

  const timer_config_t config_timer = {
    .callback = ao_timer,
    .user_data = chip,
  };
  timer_t timer = timer_init(&config_timer);
  timer_start(timer, UM_SEGUNDO, true);
}
