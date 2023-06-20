#include "stdio.h"
#include "string.h"
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "math.h"

#define BLINKER_COMPLEXITY 10

#define IN0 7
#define IN1 10
#define IN2 11
#define OUT0 12
#define OUT1 27
#define OUT2 28
#define LEDA 22
#define LEDB 21
#define SWITCH_PWR_EN 0
#define COMP_PWR_EN 1
#define MAIN_RELAY 2
#define LIGHT_A 3
#define LIGHT_B 4
#define MUX_S2 20
#define MUX_S1 19
#define MUX_S0 18
#define AUX_SW 13
#define ADC_MUX 26
#define ADC_MUX_CHANNEL 0
#define JET_ON 15
#define BUILT_IN_LED 25
#define COMP_I_MONITOR 5
#define SWITCH_I_MONITOR 6
#define mask 0xffffffe0
#define V_REF 3.25
#define PRIORITY_CONST 50000

#define InitialPower 0x00000000
#define MainRelay 0x00000004
#define CompAndSwitch 0x00000003
uint32_t current_state = 0;
uint32_t all_pins = (
  (1<<IN0) | (1<<IN1) | (1<<IN2) | (1<<OUT0) | (1<<OUT1) | 
  (1<<OUT2) | (1<<LEDA) | (1<<LEDB) | (1<<SWITCH_PWR_EN) | 
  (1<<COMP_PWR_EN) | (1<<MAIN_RELAY) | (1<<LIGHT_A) | 
  (1<<LIGHT_B) | (1<<MUX_S2) | (1<<MUX_S1) | (1<<MUX_S0) | 
  (1<<ADC_MUX) | (1<<JET_ON) | (1<<BUILT_IN_LED) | 
  (1<<COMP_I_MONITOR) | (1<<SWITCH_I_MONITOR) | (AUX_SW)
);
uint32_t output_pins = (
  (1<<OUT0) | (1<<OUT1) | (1<<OUT2) | (1<<LEDA) | 
  (1<<LEDB) | (1<<SWITCH_PWR_EN) | (1<<COMP_PWR_EN) | 
  (1<<MAIN_RELAY) | (1<<LIGHT_A) | (1<<LIGHT_B) | 
  (1<<MUX_S2) | (1<<MUX_S1) | (1<<MUX_S0) | (1<<JET_ON) |
  (1<<BUILT_IN_LED)
);
uint32_t input_pins = (
  (1<<IN0) | (1<<IN1) | (1<<IN2) | (1<<ADC_MUX) | 
  (1<<COMP_I_MONITOR) | (1<<SWITCH_I_MONITOR) | (1<<AUX_SW)
);

bool last_aux_sw_state = false;

int volt_threshold = (1 << 9);

typedef struct bit_holder{
    int S2, S1, S0;
} bits;
const bits AUX_Voltage = {0,0,1};
const bits Temp_Sensor1 = {1,1,0};
const bits Temp_Sensor2 = {1,1,1};

typedef struct process_monitor{
    bool in_process;
    uint64_t start_time;
} monitor;
monitor sd_now = {false, 0};
bool end_sd = false;
//Time in seconds between cutting power and shutdown sequence start. 
int shutdown_delay = 10; 
monitor debug = {false, 0};
monitor engage = {true, 0};
uint64_t debug_time = 0;
bool debug_force_sd = false;

typedef struct blinker{
  int pulses; 
  double times[BLINKER_COMPLEXITY];
  int states[BLINKER_COMPLEXITY];
  double length;
} blink_type;

blink_type standard_blink = {1,{0.0,0.5},{1,0},1.0};
blink_type debug_blink = {2,{0.0,0.15,0.25,0.4},{1,0,1,0},1.0};
blink_type sd_blink = {1,{0.0,0.5},{1,0},0.2};
blink_type end_sd_blink = {3,{0.0,0.05,0.1,0.15,0.2,0.25},{1,0,1,0,1,0},1.0};

void set_mux(bits pins){
  sleep_us(10);
  gpio_put(MUX_S2,pins.S2);
  gpio_put(MUX_S1,pins.S1);
  gpio_put(MUX_S0,pins.S0);
  sleep_us(10);
}

uint get_channel_from_pin(uint pin){
  if (pin == 26){
    return 0;
  } else if (pin == 27){
    return 1;
  } else if (pin == 28){
    return 2;
  } else {
    return 0;
  }
}

double convt_time(uint64_t time){
  double millis = (double)time/1000.0;
  return millis;
}

uint read_ADC_MUX(bits pins){
  adc_select_input(get_channel_from_pin(ADC_MUX));
  set_mux(pins);
  uint data = adc_read();
  return data;
}

int check_pow(){
  uint voltage = read_ADC_MUX(AUX_Voltage);
  if (voltage > volt_threshold){
    return 1;
  }
  else {
    return 0;
  }
}

void evaluate_state(uint64_t time){
  time -= engage.start_time;
  bool holder = (bool)check_pow();
  if (!holder){
    sd_now.start_time = time_us_64()-debug_time;
    sd_now.in_process = true;
  }
  else if ((time > 20000000) && holder){
    current_state = MainRelay | CompAndSwitch;
  }
  else if ((time > 10000000) && holder) {
    current_state = MainRelay;
  }
  else if ((time > 0) && holder) {
    current_state = InitialPower;
  }
}

void shutdown_process(uint64_t input_time){
  uint64_t relative_time = input_time - (sd_now.start_time);
  uint64_t delay_time = (uint64_t)(1000000*shutdown_delay);
  engage.in_process = (bool)check_pow();
  if (debug_force_sd){
    engage.in_process = false;
  }
  if (engage.in_process){
    if ((input_time - engage.start_time) > 20000000){
      engage.start_time = input_time-20000000;
    }
    else if ((input_time - engage.start_time) > 10000000){
      engage.start_time = input_time - 10000000;
    }
    else {
      engage.start_time = input_time;
    }
  }
  if ((relative_time > (delay_time + 10000000))&&(!engage.in_process)){
    current_state = InitialPower;
    watchdog_enable(50,1);
    //Once this passes to the gpio_puts_masked, this will be end of program.
    //If it does not shutdown, then a watchdog is enabled
    //to force a reboot because an error has likely occured. 
  }
  else if ((relative_time > (delay_time + 1000000))&&(!engage.in_process)){
    current_state = current_state & ~(1<<JET_ON);
  }
  else if ((relative_time > delay_time)&&(!engage.in_process)){
    current_state = current_state | (1<<JET_ON);
    end_sd = true;
  }
  else if ((relative_time <= delay_time) && (engage.in_process)){
    sd_now.in_process = false;
    sd_now.start_time = 0; 
  }
}


/* This function was rendered unnecessary
by the gpio_put_masked built-in function, 
but I left it here temporarily. 

void state_enforce(uint32_t encoded_state, uint32_t gpio_pins){
    uint32_t geo_counter = 1;
    bool cond_holder;
    for (int i = 0; i <= 29; i++){
        if ((geo_counter & gpio_pins)>0){
            //Bool type casting should make any non-zero result true here.
            cond_holder = (bool)(encoded_state & geo_counter);
            gpio_put(i, cond_holder);
            printf("%d:%d ",i,cond_holder);
        }
        geo_counter *= 2;
    }
    printf("\n");
}
*/

void toggle_pin(int pin){
  uint32_t pin_mask = (1 << pin);
  current_state = current_state ^ pin_mask;
  gpio_put_masked(output_pins,current_state);
}

float convt_temp(float temp){
  //The MCP9700T is supposed to have temp coeff Vc = 10mV/˚C and V(0˚) = 400mv. 
  //The datasheet gives the output equation Vout = Va*Vc+V(0˚), where Va = 
  //the ambient temperature. 
  //4096 is based on the RPi Pico's 12-bit ADC readings
  float Vc = 10.0/1000.0;
  float V0 = 500.0/1000.0;
  temp = temp*(V_REF/4096.0);
  temp = temp-V0;
  temp = temp/Vc;
  return temp;
}

float check_temp(int sensor){
  float temp = 0.0;
  if (sensor == 1){
    temp = (float)read_ADC_MUX(Temp_Sensor1);
  }
  else if (sensor == 2){
    temp = (float)read_ADC_MUX(Temp_Sensor2);
  }
  temp = convt_temp(temp);
  return temp;
}

double current_monitor_read(int pin){
  adc_select_input(get_channel_from_pin(pin));
  uint data = adc_read();
  double voltage = data*(V_REF/4096.0);
  return ((voltage-0.23)/0.055);
}

void parser(int input_char){
  bool valid_command = false;
  switch (input_char) {
    //"M" toggles the main relay
    case 77:
      toggle_pin(MAIN_RELAY);
      valid_command = true;
    break;
    case 83:
      //"S" toggles the switch relay
      toggle_pin(SWITCH_PWR_EN);
      valid_command = true;
    break;
    //"C" toggles the computer relay
    case 67:
      toggle_pin(COMP_PWR_EN);
      valid_command = true;
    break;
    //"J" toggles the Jetson on pin
    case 74:
      toggle_pin(JET_ON);
      valid_command = true;
    break;
    //"T" prints temperatures over serial
    case 84:{
      printf("%.4f°C  ",check_temp(1));
      printf("%.4f°C\n",check_temp(2));
      valid_command = true;
    break;}
    //"a" toggles LEDA
    case 97:
      toggle_pin(LEDA);
      valid_command = true;
    break;
    //"b" toggles LEDB
    case 98:
      toggle_pin(LEDB);
      valid_command = true;
    break;
    //"A" toggles LIGHT_A
    case 65:
      toggle_pin(LIGHT_A);
      valid_command = true;
    break;
    //"B" toggles LIGHT_B
    case 66:
      toggle_pin(LIGHT_B);
      valid_command = true;
    break;
    //"O" enables output pin control parsing expects 4 chars like "O001" MSB (2) to LSB (0)
    case 79:{
      uint32_t input_string[3];
      input_string[2] = getchar()-48;
      input_string[1] = getchar()-48;
      input_string[0] = getchar()-48;
      uint32_t state_update = (input_string[0] << OUT0) + (input_string[1] << OUT1) + (input_string[2] << OUT2);
      uint32_t outputs = (1 << OUT0) + (1 << OUT1) + (1 << OUT2);
      current_state = (current_state & (~outputs))+state_update;
      gpio_put_masked(output_pins,current_state);
      printf("\n");
      valid_command = true;
    }
    break;
    //"I" enables input pin value reading
    case 73:{
      int holder[3];
      holder[0] = gpio_get(IN0);
      holder[1] = gpio_get(IN1);
      holder[2] = gpio_get(IN2);
      printf("I%1d%1d%1d\n",holder[2],holder[1],holder[0]);
      valid_command = true;
    }
    break;
    //"K" runs shutdown procedure
    case 75:
      debug.in_process = false;
      sd_now.in_process = true;
      debug_force_sd = true;
      end_sd = true;
      valid_command = true;
      break;
    //"V" reads voltage of input from ADC mux with optional choice of which
    //pin to read. Defaults to reading Pin 1 on the mux. 
    case 86:
    {
      int holder = getchar_timeout_us(5000000);
      if (holder == PICO_ERROR_TIMEOUT){
        uint voltage = read_ADC_MUX(AUX_Voltage);
        printf("%d\n",voltage);
      }
      else{
        if ((holder >= 48)&&(holder <= 57)){
          holder -= 48;
          bits selector = {0,0,0};
          selector.S2 = ((holder&4) >> 2);
          selector.S1 = ((holder&2) >> 1);
          selector.S0 = (holder&1);
          uint voltage = read_ADC_MUX(selector);
          printf("MUX Pin %d: %d\n",holder,voltage);
        }
      }
      valid_command = true;
    }
    
    break;
    case 100:
      debug.in_process = false;
      valid_command = true;
      break;
    }
  if (valid_command){
    printf("Input \"%c\": done\n", input_char);
  }
}

void check_aux_switch(){
  gpio_set_pulls(AUX_SW,true,false);
  bool holder = gpio_get(AUX_SW);
  if ((holder != last_aux_sw_state) && (holder == false)){
    printf("Auxiliary switch pressed. That's neat!\n");
    last_aux_sw_state = false;
  } else {
    last_aux_sw_state = holder;
  }
}

void blink_pattern(){
  int state_changes;
  double time = (double)(time_us_64())/1000000.0;
  double time_in_pulse;
  blink_type holder;
  if (debug.in_process){
    state_changes = debug_blink.pulses * 2;
    time_in_pulse = time / debug_blink.length;
    time_in_pulse = (double)(time_in_pulse - (double)floor(time_in_pulse))*debug_blink.length;
    holder = debug_blink;
  } 
  else if (end_sd){
    state_changes = end_sd_blink.pulses *2;
    time_in_pulse = time / end_sd_blink.length;
    time_in_pulse = (double)(time_in_pulse - (double)floor(time_in_pulse))*end_sd_blink.length;
    holder = end_sd_blink;
  }
  else if (sd_now.in_process){
    state_changes = sd_blink.pulses *2;
    time_in_pulse = time / sd_blink.length;
    time_in_pulse = (double)(time_in_pulse - (double)floor(time_in_pulse))*sd_blink.length;
    holder = sd_blink;
  }
  else {
    state_changes = standard_blink.pulses * 2;
    time_in_pulse = time / standard_blink.length;
    time_in_pulse = (double)(time_in_pulse - (double)floor(time_in_pulse))*standard_blink.length;
    holder = standard_blink;
  }
  for (int i = (state_changes-1); i >=0; i--){
      //printf("%f   %f   %d   i:%d\n",(holder.times[i]*holder.length),time_in_pulse, holder.states[i],i);
      if (time_in_pulse > (holder.times[i]*holder.length)){
        current_state = current_state & (~(1 << BUILT_IN_LED));
        current_state = current_state | ((holder.states[i]) << BUILT_IN_LED);
        break;
      }
    }
}

uint64_t debug_mode(){
    printf("Ready!\n");
    while (debug.in_process) {
        int holder;
        holder = getchar_timeout_us(0);
        if (holder != -1){
            parser(holder);
        }
        blink_pattern();
        gpio_put_masked(output_pins,current_state);
        check_aux_switch();
    }
    printf("Exiting debug mode\n");
    return time_us_64() - debug_time;
}

int main(){
  stdio_init_all();
  //Found these neat predefined functions in the SDK
  //for setting pins from a bitmap.
  gpio_init_mask(all_pins);
  gpio_set_dir_out_masked(output_pins);
  gpio_set_dir_in_masked(input_pins);
  //Configuring ADC input is separate
  adc_init();
  adc_gpio_init(ADC_MUX);
  bool checked_priority = false;
  while (1) {
    uint64_t time_ref = time_us_64() - debug_time;
    if (debug.in_process) {
      bool holder = sd_now.in_process; 
      printf("Entering debug mode\n");
      debug.start_time = time_ref;
      debug_time += debug_mode() - debug.start_time;
      if ((!holder)&&(sd_now.in_process)){
        sd_now.start_time = time_us_64()-debug_time;
      }
    }
    if (((time_ref % PRIORITY_CONST)>(PRIORITY_CONST/2)) && (!checked_priority)){
      if (!sd_now.in_process){
        evaluate_state(time_ref);
      }
      else {
        shutdown_process(time_ref);
      }
      check_aux_switch();
      checked_priority = true;
    } else if ((time_ref % PRIORITY_CONST)<(PRIORITY_CONST/4)){
      checked_priority = false;
    }
    blink_pattern();
    gpio_put_masked(output_pins,current_state);
    int char_holder = getchar_timeout_us(0);
    if (char_holder==100){
      debug.in_process = true;
    } else if (char_holder == 84){
      printf("Reference time: %lu\n", (uint32_t)(time_ref&0xffffffff));
    } else if (char_holder == 82){
      //This has the effect of resetting time references
      debug_time = time_us_64();
    }
  }
  //Code should NEVER go beyond here. If it does, reboot. 
  watchdog_enable(1,1);
}