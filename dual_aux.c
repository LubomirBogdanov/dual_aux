/*
 * File:   main.c
 * Author: lbogdanov
 *
 * Created on March 19, 2026, 11:32 PM
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <xc.h>
#include <pic16f18324.h>

// CONFIGURATION BITS
#pragma config FEXTOSC = HS
#pragma config RSTOSC = HFINT1   
#pragma config WDTE = OFF
#pragma config PWRTE = ON
#pragma config MCLRE = ON
#pragma config CP = OFF
#pragma config CPD = OFF
#pragma config BOREN = ON
#pragma config CLKOUTEN = OFF

#define _XTAL_FREQ 4000000

#define RX_BUFF_LEN         32
#define IDN_STRING          "\nDualAUX,hw1.0,sw1.0\n\r"
#define CH0_SELECTED_STRING "\nCH0 selected!\n"
#define CH1_SELECTED_STRING "\nCH1 selected!\n"
#define DEBOUNCE_DELAY_MS   100

void h_bridge_set(uint8_t value);
void leds_set(uint8_t led_number, uint8_t led_value);
void relay_set(uint8_t relay_value);
void UART_Write(char data);

char rx_buff[RX_BUFF_LEN];
volatile uint8_t cmd_received = 0;
volatile uint8_t char_received = 0;
volatile uint8_t selected_channel = 0;
volatile uint8_t selected_channel_by_key = 2;
volatile uint8_t index = 0;
volatile uint8_t timeout_count = 0;
volatile uint8_t save_to_eeprom = 0;

/*!
* \brief The peripheral interrupt service handler.
*
* The served interrupts are from: UART, GPIO and Timer1.
* No work is done here for the GPIO and Timer1 peripherals.
* Work is done for the UART - the command message is 
* constructed from the incoming chars. The resulting message
* is processed in main.
*
* \return None.
*/
void __interrupt() ISR(void){
    char ch;
     
    if (PIR0bits.IOCIF){
        if (IOCCFbits.IOCCF4){
            IOCCFbits.IOCCF4 = 0;
            selected_channel_by_key = 0;
        }
        
        if (IOCAFbits.IOCAF0){
            IOCAFbits.IOCAF0 = 0;  
            selected_channel_by_key = 1;            
        }

        PIR0bits.IOCIF = 0;
    }
    
    if (PIE1bits.RCIE && PIR1bits.RCIF){
        //Reset if errors ocurred
        if (RCSTAbits.OERR) {
            RCSTAbits.CREN = 0;
            RCSTAbits.CREN = 1;
        }

        ch = RCREG;
        
        char_received = ch;        
            
        rx_buff[index] = ch;
        index++;
            
        if(index == (RX_BUFF_LEN-1)){
            cmd_received = 1;                
        }
            
        if(ch == '\n' || ch == '\r'){
            cmd_received = 1;
        }            
            
        if(cmd_received){
            rx_buff[index] = '\0';
            index = 0;
        }
    }
    
    if (PIR1bits.TMR1IF){
        PIR1bits.TMR1IF = 0;

        timeout_count++;

        if (timeout_count >= 6){
            timeout_count = 0;
            T1CONbits.TMR1ON = 0; 
            save_to_eeprom = 1;
        }
    }
}

/*!
* \brief The UART initialization function: 9600-8-N-1
*
* The UART module has been setup for operation at 9600 
* bauds, 8 data bits, 1 stop bit and no parity bit.
* Interrupts are signalled on every character reception.
* The interface is used to receive commands over USB-UART
* converter.
*
* This function is ChatGPT generated.
*
* \return None.
*/
void uart_init(void){
    TRISCbits.TRISC1 = 0; //RC1 = output
    TRISCbits.TRISC2 = 1; //RC2 = input
    RC1PPS = 0x14; //EUSART TX = RC1
    RXPPS  = 0x12; //EUSART RX = RC2
    
    BAUDCONbits.BRG16 = 1; //16-bit baud generator
    TXSTAbits.BRGH = 1; //High-speed mode

    // SPBRG = (Fosc / (4 * Baud)) - 1
    // = (4,000,000 / (4 * 9600)) - 1 ? 103
    SPBRGH = 0;
    SPBRGL = 103;

    RCSTAbits.SPEN = 1; //Enable UART
    TXSTAbits.TXEN = 1; //Enable transmitter
    RCSTAbits.CREN = 1; //Enable receiver
    
    PIE1bits.RCIE = 1; //RX interrupt enable
    PIE1bits.TXIE = 0; //TX interrupt disabled for now    
}

/*!
* \brief Send a single character over UART.
*
* This function waits for the previous character to
* get sent. It then sends a new one. This is a blocking
* function.
*
* \param data - an 8-bit ASCII character to be sent over
* the UART TxD pin.
*
* \return None.
*/
void uart_write(char data){
    while (!PIR1bits.TXIF);
    TXREG = data;
}

/*!
* \brief Initialize GPIO to control an H-bridge.
*
* This functions initializes two outputs (RA2 and
* RC0) for the H-bridge control. An H-bridge is 
* needed because the relay is bistable and needs
* bipolar voltages.
*
* \return None.
*/
void h_bridge_init(void){
    TRISCbits.TRISC0 = 0; 
    TRISAbits.TRISA2 = 0;
    LATAbits.LATA2 = 0; 
    LATCbits.LATC0 = 0; 
}

/*!
* \brief Used for toggle and turning off of the relay.
*
* This function toggles the relay between one of its 
* DPST positions. It also has the ability to turn off
* the supply of the relay to supply power.
*
* \param value - 0 selects PHA=1+PHB=0, 1 selects 
* PHA=0+PHB=1 and 2 selects the off state PHA=0+PHB=0.
*
* \return None.
*/
void h_bridge_set(uint8_t value){
    switch(value){
        case 0:
            LATAbits.LATA2 = 1; 
            LATCbits.LATC0 = 0; 
            break;
        case 1:
            LATAbits.LATA2 = 0; 
            LATCbits.LATC0 = 1; 
            break;
        case 2:
            LATAbits.LATA2 = 0; 
            LATCbits.LATC0 = 0; 
            break;
    }
}

/*!
* \brief A function to send a sequence of characters
* over the UART interface. 
*
* This function sends a string of characters over the
* UART's TxD pin. It stops upon finding a NULL character.
*
* \return None.
*/
void print_line(char *str){
   uint8_t index = 0;
    
   while (1){
       if(str[index] == '\0'){
           break;
       }
       uart_write(str[index]);
       index++;
   }
}

/*!
* \brief Initializes RC3 and RC5 as GPIO outputs for LEDs control.
*
* This functions intializes two GPIO pins as outpus to control the 
* integrated LEDs of the buttons. When pressed, the button should be
* lit up.
*
* \return None.
*/
void leds_init(void){
    TRISCbits.TRISC5 = 0; 
    TRISCbits.TRISC3 = 0;
    LATCbits.LATC5 = 0;
    LATCbits.LATC3 = 0;
}

/*!
* \brief A function to set 0 or 1 to the LED pins.
*
* This function sets logic 0 (0V) or logic 1 (5V) on one of the LED 
* pins that in turn lights up the respective LED.
*
* \param led_number - selects the diode to be turned on/off.
* The number 0 is RC5, 1 is RC3.
* \param led_value - 0 turns off the LED, 1 turns on the LED.
*
* \return None.
*/
void leds_set(uint8_t led_number, uint8_t led_value){
    switch(led_number){
        case 0:
            LATCbits.LATC5 = led_value; 
            break;
        case 1:
            LATCbits.LATC3 = led_value; 
            break;        
    }
}

/*!
* \brief A high-level function to implement triple LED blink
* to denote the saving of the pressed button to the EEPROM.
*
* When the user presses a button, a timer delay of 3 seconds is 
* started. When the timer times out, the state of the selected
* channel is saved to EEPROM.
*
* \param led_number - 0 is the LED on RC5, 1 is the LED on RC3.
* \param num_of_blinks - a number between 0 and 255 to control
* the number of the LED blinks.
*
* \return None.
*/
void leds_blink(uint8_t led_number, uint8_t num_of_blinks){
    uint8_t i;
    
    for(i = 0; i < num_of_blinks; i++){
        leds_set(led_number,1);
        __delay_ms(100);
        leds_set(led_number,0);
        __delay_ms(100);
    } 
    
    leds_set(led_number,1);
}

/*!
* \brief A function to initialize two GPIO pins as inputs for  
* implementing buttons.
*
* This function initializes the pins where the buttons are connected.
* These pins are RA0 and RC4. Because the buttons are connected to 
* ground, without pull-ups, the internal pull-ups are turned on as 
* well for each pin.
*
* This function is ChatGPT generated.
*
* \return None.
*/
void buttons_init(void){
    TRISCbits.TRISC4 = 1;  
    TRISAbits.TRISA0 = 1;  

    WPUAbits.WPUA0 = 1;   
    WPUCbits.WPUC4 = 1;  
    
    IOCANbits.IOCAN0 = 1; 
    IOCCNbits.IOCCN4 = 1;   
    IOCAPbits.IOCAP0 = 0;
    IOCCPbits.IOCCP4 = 0;
    IOCAFbits.IOCAF0 = 0;
    IOCCFbits.IOCCF4 = 0;
    
    PIE0bits.IOCIE = 1;   
}

/*!
* \brief A function for reading the button pins' state.
*
* Even though the button pins use interrupts, this 
* function is needed for the debouncing function of each
* button. Upon button press, an interrupt is generated
* that raises a flag. In main this flag is read and a 
* software delay is started. After the delay, the button's
* pin is read again and if it is still logic 0, the button
* is considered as pressed.
*
* \param button_number - can be 0 or 1 for the respective
* button.
*
* \return 0 - the button is pressed, 1 - the button is 
* released.
*/
uint8_t buttons_read(uint8_t button_number){
    uint8_t value;
    
    switch(button_number){
        case 0:
            value = PORTCbits.RC4;
            break;
        case 1:
            value = PORTAbits.RA0;
            break;
    }
    
    return value;
}

/*!
* \brief A function to convert an integer to a string.
*
* This function takes one byte and converts it to a 4-char
* string. The first two characters are ASCII characters 
* in the range of 0 - 9 and A - F. The third character
* is the ASCII line feed. The fourth character is a 
* terminating NULL.
*
* This function is ChatGPT generated.
*
* \param data_byte - the byte to be converted (can be 0 - 255).
* \param hex_str [out] - a pointer to an array that will hold
* the resulting string.
*
* \return None.  
*/
void byte_to_hex(uint8_t data_byte, char *hex_str) {
    const char hex_chars[] = "0123456789ABCDEF";

    hex_str[0] = hex_chars[(data_byte >> 4) & 0x0F]; 
    hex_str[1] = hex_chars[data_byte & 0x0F];       
    hex_str[2] = '\n';
    hex_str[3] = '\0';        
}

void relay_set(uint8_t relay_value){
    if(relay_value == 0){
        selected_channel = 0;
        h_bridge_set(0);
        __delay_ms(50);
        h_bridge_set(2);
        leds_set(0, 1);
        leds_set(1, 0);
    }
    
    if(relay_value == 1){
        selected_channel = 1;
        h_bridge_set(1);
        __delay_ms(50);
        h_bridge_set(2);
        leds_set(0, 0);
        leds_set(1, 1);  
    }
}

/*!
* \brief A function to read one byte from the first
* addess of the EEPROM memory.
*
* This function reads the value stored in address 0x7000.
* This is the first address of the EEPROM memory. Note
* that MPLAB X IDE erases the EEPROM upon program flashing.
* If you want to retain the state of the EEPROM, select
* the option "Preserve EEPROM Memory" from the debugger's 
* menu.
*
* \return The value stored in the first memory location 
* of EEPROM. The number ranges 0 - 255.
*/
uint8_t read_eeprom_r(void){
    uint8_t result = 0;
    
    NVMCON1bits.NVMREGS = 1;

    NVMADRL = 0x00;
    NVMADRH = 0x70;
    NVMCON1bits.RD = 1;
    
    while(NVMCON1bits.RD);
    
    result = NVMDATL;
    
    return result;
}

/*!
* \brief A function to write a value to the EEPROM memory.
*
* This function writes an 8-bit value to the first memory
* location of the EEPROM. It can be paired with read_eeprom_r().
* 
* \param value - a number between 0 and 255 to be written to the
* EEPROM.
*
* \return None.
*/
void write_eeprom_r(uint8_t value){
    NVMCON1bits.NVMREGS = 1;   // EEPROM
    NVMADRL = 0x00;
    NVMADRH = 0x70;

    NVMDATL = value;

    NVMCON1bits.WREN = 1; //Allow programming of EEPROM
    NVMCON1bits.LWLO = 0; //The next WR=1 will write to EEPROM

    INTCONbits.GIE = 0;

    NVMCON2 = 0x55;
    NVMCON2 = 0xAA;
    NVMCON1bits.WR = 1;

    while(NVMCON1bits.WR);     // wait complete

    INTCONbits.GIE = 1;

    NVMCON1bits.WREN = 0;
}

/*!
* \brief An initialization function for  Timer 1 to
* implement a 3-second timeout delay.
*
* When a button is pressed, a 3-second delay is started.
* When a timeout occurrs, the selected channel's value
* is written to EEPROM. To implement this, the timer in 
* free running mode is started. It takes around 6 overflows
* to accomplish approximately a 3 second delay.
*
* This function is ChatGPT generated.
*
* return None.
*/
void timer1_init(void){
    T1CON = 0x30;
    // bit7-6: clock source = Fosc/4
    // bit5-4: prescaler 1:8
    // bit0: Timer OFF
    TMR1 = 0;
    PIR1bits.TMR1IF = 0;
    PIE1bits.TMR1IE = 1;
}

/*!
* \brief A function to start and restart the Timer 1 timeout.
*
* This function starts the timer counting. It enables the timer's
* interrupts also.
*
* \return None.
*/
void timer1_start(void){
    T1CONbits.TMR1ON = 0;  // stop timer first

    timeout_count = 0;
            
    TMR1H = 0;
    TMR1L = 0;

    PIR1bits.TMR1IF = 0;
    T1CONbits.TMR1ON = 1;  
}

/*!
* \brief The entry point of the microcontroller's firmware.
*
* This function implements a dual-input to single output
* audio switching device. It can be UART interface controlled, 
* or via a two-button keyboard. Its state can be monitored over 
* the UART interface. A USB-to-UART converter IC has been used,
* so the interface that connects to the host PC is actually
* USB.
*
* return None. This is a blocking function and never exits.
*/
void main(void){
    uint8_t state;
    int match;
    char selected_channel_str[4];
    
    OSCCON1 = 0x70; //External OSC=4MHz, div=1
    
    ANSELA = 0; //Disable analog function on PORTA
    ANSELC = 0; //Disable analog function on PORTC

    uart_init();
    leds_init();
    buttons_init();
    timer1_init();
    h_bridge_init();    
    
    __delay_ms(1);

    selected_channel = read_eeprom_r();
    
    if(selected_channel != 0 && selected_channel != 1){
        print_line("EEPROM read error! Defaulting to CH0!\n"); 
        byte_to_hex(selected_channel, selected_channel_str);
        print_line(selected_channel_str);
        selected_channel = 0;
        write_eeprom_r(selected_channel);
    }
    
    relay_set(selected_channel);
    
    byte_to_hex(selected_channel, selected_channel_str);
    
    print_line(IDN_STRING);    
    print_line("Selected channel: ");
    print_line(selected_channel_str);    
    
    INTCONbits.PEIE = 1;  // Peripheral interrupts
    INTCONbits.GIE = 1;   // Global interrupts
    
    while (1){
        if(char_received){
            uart_write(char_received);
            char_received = 0;
        }
        
        if(selected_channel_by_key == 0 || selected_channel_by_key == 1){
            __delay_ms(DEBOUNCE_DELAY_MS);
            
            state = buttons_read(selected_channel_by_key);           
            
            if(state == 0){                
                if(selected_channel_by_key == 0){
                    print_line(CH0_SELECTED_STRING);
                }
                
                if(selected_channel_by_key == 1){
                    print_line(CH1_SELECTED_STRING);
                }
                
                relay_set(selected_channel_by_key);
                timer1_start();
            }
            
            selected_channel_by_key = 2;
        }       
        
        if(cmd_received){      
            cmd_received = 0;           
            
            match = strcmp("CH0\n", rx_buff);
            if(match == 0){
                print_line(CH0_SELECTED_STRING);    
                relay_set(0);
                timer1_start();
                continue;
            }

            match = strcmp("CH1\n", rx_buff);
            if(match == 0){
                print_line(CH1_SELECTED_STRING);    
                relay_set(1);
                timer1_start();
                continue;
            }
            
            match = strcmp("*IDN?\n", rx_buff);
            if(match == 0){
                print_line(IDN_STRING);  
                continue;
            }
                                   
            match = strcmp("CH?\n", rx_buff);
            if(match == 0){
                if(selected_channel == 0){
                    print_line(CH0_SELECTED_STRING);  
                }
                
                if(selected_channel == 1){
                    print_line(CH1_SELECTED_STRING);                      
                }
                continue;
            }
            
            match = strcmp("*RST\n", rx_buff);
            if(match == 0){
                asm("reset");
                while(1){ }
            }

            if(match){
                print_line("\nWrong command!\n");   
            }
        }

        if(save_to_eeprom){
            save_to_eeprom = 0;            
            
            state = read_eeprom_r();
            
            if(state != selected_channel){ 
                print_line("\nSaving in EEPROM ...\n"); 
                write_eeprom_r(selected_channel);  
            }            
            
            leds_blink(selected_channel, 3);
        }
    }
}
