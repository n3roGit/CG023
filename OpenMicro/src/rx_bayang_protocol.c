/*
The MIT License (MIT)

Copyright (c) 2016 silverx

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#include "binary.h"
#include "drv_spi.h"

#include "project.h"
#include "xn297.h"
#include "drv_time.h"
#include <stdio.h>
#include "config.h"
#include "defines.h"

#include "rx_bayang.h"

#include "util.h"

#ifdef RX_BAYANG_PROTOCOL



// packet period in uS
#define PACKET_PERIOD 3300

// was 250 ( uS )
#define PACKET_OFFSET 500

#ifdef USE_STOCK_TX
#undef PACKET_OFFSET
#define PACKET_OFFSET 250
#endif

// how many times to hop ahead if no reception
#define HOPPING_NUMBER 4


//#define RXDEBUG

#define BAYANG_LOWRATE_MULTIPLIER 0.5


// compatibility with older version hardware.h
#if ( !defined RADIO_XN297 && !defined RADIO_XN297L)
#define RADIO_XN297
#endif




extern float rx[4];
extern char aux[AUXNUMBER];
extern char lastaux[AUXNUMBER];
extern char auxchange[AUXNUMBER];

// global variables
int failsafe = 0;
int rxmode = 0;


void writeregs ( uint8_t data[] , uint8_t size )
{
spi_cson();
for ( uint8_t i = 0 ; i < size ; i++)
{
	spi_sendbyte( data[i]);
}
spi_csoff();
delay(1000);
}


void rx_init()
{

#ifdef RADIO_XN297
static uint8_t bbcal[6] = { 0x3f , 0x4c , 0x84 , 0x6F , 0x9c , 0x20  };
writeregs( bbcal , sizeof(bbcal) );
// new values
static uint8_t rfcal[8] = { 0x3e , 0xc9 , 0x9a , 0xA0 , 0x61 , 0xbb , 0xab , 0x9c };
writeregs( rfcal , sizeof(rfcal) );

static uint8_t demodcal[6] = { 0x39 , 0x0b , 0xdf , 0xc4 , 0xa7 , 0x03};
writeregs( demodcal , sizeof(demodcal) );
#endif

static int rxaddress[5] = { 0 , 0 , 0 , 0 , 0  };
xn_writerxaddress( rxaddress);

	xn_writereg( EN_AA , 0 );	// aa disabled
	xn_writereg( EN_RXADDR , 1 ); // pipe 0 only
	xn_writereg( RF_SETUP , B00000001);  // lna high current on ( better performance )
	xn_writereg( RX_PW_P0 , 15 ); // payload size
	xn_writereg( SETUP_RETR , 0 ); // no retransmissions ( redundant?)
	xn_writereg( SETUP_AW , 3 ); // address size (5 bits)
	xn_command( FLUSH_RX);
  xn_writereg( RF_CH , 0 );  // bind on channel 0

#ifdef RADIO_XN297
  xn_writereg( 0 , B00001111 ); // power up, crc enabled
#endif

#ifdef RADIO_XN297L
  xn_writereg( 0 , B10001111 ); // power up, crc enabled
#endif

#ifdef RADIO_CHECK
void check_radio(void);
 check_radio();
#endif	
}


void check_radio()
{	
	int temp = xn_readreg( 0x0f); // rx address pipe 5	
	// should be 0xc6
	extern void failloop( int);
	if ( temp != 0xc6) failloop(3);
}


static char checkpacket()
{
	spi_cson();
	int status = spi_sendzerorecvbyte();
	spi_csoff();
	if ( status&(1<<MASK_RX_DR) )
	{	 // rx clear bit
		// this is not working well
	 // xn_writereg( STATUS , (1<<MASK_RX_DR) );
		//RX packet received
		//return 1;
	}
	if( (status & B00001110) != B00001110 )
	{
		// rx fifo not empty		
		return 2;	
	}
	
  return 0;
}



float packettodata( int *  data)
{
	return ( ( ( data[0]&0x0003) * 256 + data[1] ) - 512 ) * 0.001953125 ;	
}

// internal variables
char rfchannel[4];
int chan = 0;
int rxdata[15];
unsigned long lastrxtime;
unsigned long failsafetime;
unsigned int skipchannel = 0;
int lastrxchan;
int timingfail = 0;

static int decodepacket( void)
{
	if ( rxdata[0] == 165 )
	{
		 int sum = 0;
		 for(int i=0; i<14; i++) 
		 {
			sum += rxdata[i];
		 }	
		if ( (sum&0xFF) == rxdata[14] )
		{
			rx[0] = packettodata( &rxdata[4] );
			rx[1] = packettodata( &rxdata[6] );
			rx[2] = packettodata( &rxdata[10] );
		// throttle		
			rx[3] = ( (rxdata[8]&0x0003) * 256 + rxdata[9] ) * 0.000976562;
		
#ifndef DISABLE_EXPO
	rx[0] = rcexpo ( rx[0] , EXPO_XY );
	rx[1] = rcexpo ( rx[1] , EXPO_XY ); 
	rx[2] = rcexpo ( rx[2] , EXPO_YAW ); 	
#endif
		if  (rxdata[1] != 0xfa) 
		{// low rates
			for ( int i = 0 ; i <3; i++)
			{
				rx[i] = rx[i] * (float) BAYANG_LOWRATE_MULTIPLIER;
			}
		}
		// trims are 50% of controls at max		
	// trims are not used because they interfere with dynamic trims feature of devo firmware
			
//			rx[0] = rx[0] + 0.03225 * 0.5 * (float)(((rxdata[4])>>2) - 31);
//			rx[1] = rx[1] + 0.03225 * 0.5 * (float)(((rxdata[6])>>2) - 31);
//			rx[2] = rx[2] + 0.03225 * 0.5 * (float)(((rxdata[10])>>2) - 31);
	
		  // flip channel
			aux[CH_FLIP] = (rxdata[2] &  0x08)?1:0;
	
		  // expert mode
			aux[CH_EXPERT] = (rxdata[1] == 0xfa)?1:0;
	
		  // headless channel
		  aux[CH_HEADFREE] = (rxdata[2] &  0x02)?1:0;

			// rth channel
			aux[CH_RTH] = (rxdata[2] &  0x01)?1:0;

			aux[CH_INV] = (rxdata[3] & 0x80)?1:0; // inverted flag
						
			aux[CH_VID] = (rxdata[2] & 0x10) ? 1 : 0;
												
			aux[CH_PIC] = (rxdata[2] & 0x20) ? 1 : 0;			


			for ( int i = 0 ; i < AUXNUMBER - 2 ; i++)
			{
				auxchange[i] = 0;
				if ( lastaux[i] != aux[i] ) auxchange[i] = 1;
				lastaux[i] = aux[i];
			}
			
			return 1;	// valid packet	
		}
	 return 0; // sum fail
	}
return 0; // first byte different
}



void nextchannel()
{
	chan++;
	if (chan > 3 ) chan = 0;
	xn_writereg(0x25, rfchannel[chan] );
}



#ifdef RXDEBUG	

struct rxdebug rxdebug;

int packetrx;
unsigned long secondtimer;
int skipstats[HOPPING_NUMBER+2];
int afterskip[HOPPING_NUMBER+2];
#warning "RX debug enabled"

#endif



void checkrx(void)
{
	int packetreceived = checkpacket();
	int pass = 0;
	if (packetreceived)
	  {
		  if (rxmode == RXMODE_BIND)
		    {		// rx startup , bind mode
			    xn_readpayload(rxdata, 15);

			    if (rxdata[0] == 164)
			      {	// bind packet
				      rfchannel[0] = rxdata[6];
				      rfchannel[1] = rxdata[7];
				      rfchannel[2] = rxdata[8];
				      rfchannel[3] = rxdata[9];
							
							int rxaddress[5];
				      rxaddress[0] = rxdata[1];
				      rxaddress[1] = rxdata[2];
				      rxaddress[2] = rxdata[3];
				      rxaddress[3] = rxdata[4];
				      rxaddress[4] = rxdata[5];
				      
				      xn_writerxaddress(rxaddress);
				      xn_writereg(0x25, rfchannel[chan]);	// Set channel frequency 
							rxmode = RXMODE_NORMAL;

#ifdef SERIAL
				      printf(" BIND \n");
#endif
			      }
		    }
		  else
		    {		// normal mode  
#ifdef RXDEBUG
			    rxdebug.channelcount[chan]++;
			    rxdebug.packettime = gettime() - lastrxtime;
					
					if ( skipchannel&& !timingfail ) afterskip[skipchannel]++;
					if ( timingfail ) afterskip[0]++;

#endif

unsigned long temptime = gettime();
	
			    nextchannel();

			    xn_readpayload(rxdata, 15);
			    pass = decodepacket();

			    if (pass)
			      {
#ifdef RXDEBUG
				      packetrx++;
#endif
							skipchannel = 0;
							timingfail = 0;
							lastrxchan = chan;
							lastrxtime = temptime;
				      failsafetime = temptime;
				      failsafe = 0;
			      }
			    else
			      {
#ifdef RXDEBUG
				      rxdebug.failcount++;
#endif
			      }

		    }		// end normal rx mode

	  }			// end packet received

		
	unsigned long time = gettime();

		

	// sequence period 12000
	if (time - lastrxtime > (HOPPING_NUMBER*PACKET_PERIOD + 1000) && rxmode != RXMODE_BIND)
	  {			
			//  channel with no reception   
		  lastrxtime = time;
			// set channel to last with reception
			if (!timingfail) chan = lastrxchan;
			// advance to next channel
		  nextchannel();
			// set flag to discard packet timing
			timingfail = 1;
	  }
		
	if ( !timingfail && skipchannel < HOPPING_NUMBER+1 && rxmode != RXMODE_BIND)
		{
			unsigned int temp = time - lastrxtime ;

			if ( temp > 1000 && ( temp + (PACKET_OFFSET) )/((int) PACKET_PERIOD) >= (skipchannel + 1) ) 
			{
				nextchannel();
#ifdef RXDEBUG				
				skipstats[skipchannel]++;
#endif				
				skipchannel++;
			}
		}	
	
	if (time - failsafetime > FAILSAFETIME)
	  {	//  failsafe
		  failsafe = 1;
		  rx[0] = 0;
		  rx[1] = 0;
		  rx[2] = 0;
		  rx[3] = 0;
	  }
#ifdef RXDEBUG
	if (gettime() - secondtimer > 1000000)
	  {
		  rxdebug.packetpersecond = packetrx;
		  packetrx = 0;
		  secondtimer = gettime();
	  }
#endif

}

#endif





