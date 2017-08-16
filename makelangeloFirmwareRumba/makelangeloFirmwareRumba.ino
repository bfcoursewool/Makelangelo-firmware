//------------------------------------------------------------------------------
// Makelangelo - a mural drawing robot
// dan@marginallycelver.com 2013-12-26
// Copyright at end of file.  Please see
// http://www.github.com/MarginallyClever/Makelangelo for more information.
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// INCLUDES
//------------------------------------------------------------------------------
#include "configure.h"

#include <SPI.h>  // pkm fix for Arduino 1.5

#include "Vector3.h"
#include "sdcard.h"


//------------------------------------------------------------------------------
// GLOBALS
//------------------------------------------------------------------------------

// robot UID
int robot_uid=0;

// plotter limits, relative to the center of the plotter.
float limit_ymax = 0;  // distance to top of drawing area.
float limit_ymin = 0;  // Distance to bottom of drawing area.
float limit_xmax = 0;  // Distance to right of drawing area.
float limit_xmin = 0;  // Distance to left of drawing area.

static float homeX=0;
static float homeY=0;

// length of belt when weights hit limit switch
float calibrateRight  = 101.1;
float calibrateLeft   = 101.1;
float calibrateBRight = 101.1;
float calibrateBLeft  = 101.1;

// what are the motors called?
extern const char *motorNames;

// motor inversions
char m1i= 1;
char m2i=-1;
char m4i= 1;
char m5i=-1;

// calculate some numbers to help us find feed_rate
float pulleyDiameter = 4.0f / PI;  // cm; 20 teeth * 2mm per tooth / PI
float threadPerStep  = 4.0f / STEPS_PER_TURN;  // pulleyDiameter * PI / STEPS_PER_TURN

// plotter position.
float posx, posy, posz;  // pen state
float feed_rate=DEFAULT_FEEDRATE;
float acceleration=DEFAULT_ACCELERATION;
float step_delay;

char absolute_mode=1;  // absolute or incremental programming mode?

// Serial comm reception
char serialBuffer[MAX_BUF+1];  // Serial buffer
int sofar;               // Serial buffer progress
static long last_cmd_time;    // prevent timeouts


Vector3 tool_offset[NUM_TOOLS];
int current_tool=0;


long line_number=0;


//------------------------------------------------------------------------------
// METHODS
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// calculate max velocity, threadperstep.
void adjustPulleyDiameter(float diameter) {
  Serial.print(F("adjustPulleyDiameter "));
  pulleyDiameter = diameter;
  float circumference = pulleyDiameter*PI;  // circumference
  threadPerStep = circumference/(float)STEPS_PER_TURN;  // thread per step
  Serial.print(F("dia="));  Serial.println(diameter);
  Serial.print(F("cir="));  Serial.println(circumference);
  Serial.print(F("SPT="));  Serial.println((float)STEPS_PER_TURN);
  Serial.print(F("tps*1000="));  Serial.println(threadPerStep*1000.0f);
}


//------------------------------------------------------------------------------
// returns angle of dy/dx as a value from 0...2PI
float atan3(float dy,float dx) {
  float a=atan2(dy,dx);
  if(a<0) a=(PI*2.0)+a;
  return a;
}


//------------------------------------------------------------------------------
char readSwitches() {
#ifdef USE_LIMIT_SWITCH
  // get the current switch state
  return ( (digitalRead(LIMIT_SWITCH_PIN_LEFT)==LOW) | (digitalRead(LIMIT_SWITCH_PIN_RIGHT)==LOW) );
#else
  return 0;
#endif  // USE_LIMIT_SWITCH
}


//------------------------------------------------------------------------------
// feed rate is given in units/min and converted to cm/s
void setFeedRate(float v1) {
  if( feed_rate != v1 ) {
    feed_rate = v1;
    if(feed_rate > MAX_FEEDRATE) feed_rate = MAX_FEEDRATE;
    if(feed_rate < MIN_FEEDRATE) feed_rate = MIN_FEEDRATE;
#ifdef VERBOSE
    Serial.print(F("F="));
    Serial.println(feed_rate);
#endif
  }
}

void findStepDelay() {
  step_delay = 1000000.0f/DEFAULT_FEEDRATE;
}


/** 
 * delay in microseconds 
 */
void pause(long us) {
  delay(us / 1000);
  delayMicroseconds(us % 1000);
}


/**
 * print the current feed rate
 */
void printFeedRate() {
  Serial.print(F("F"));
  Serial.print(feed_rate);
  Serial.print(F("steps/s"));
}


/**
 * Inverse Kinematics turns XY coordinates into lengths L1,L2
 * @param x cartesian coordinate
 * @param y cartesian coordinate
 * @param motorStepArray a measure of each belt to that plotter position
 */
void IK(float x, float y, long *motorStepArray) {
#ifdef COREXY
  motorStepArray[0] = lround((x+y) / threadPerStep);
  motorStepArray[1] = lround((x-y) / threadPerStep);
#endif
#ifdef TRADITIONALXY
  motorStepArray[0] = lround((x) / threadPerStep);
  motorStepArray[1] = lround((y) / threadPerStep);
#endif
#ifdef POLARGRAPH2
  float dy,dx;
  // find length to M1
  dy = y - limit_ymax;
  dx = x - limit_xmin;
  motorStepArray[0] = lround( sqrt(dx*dx+dy*dy) / threadPerStep );
  // find length to M2
  dx = limit_xmax - x;
  motorStepArray[1] = lround( sqrt(dx*dx+dy*dy) / threadPerStep );
#endif
#ifdef ZARPLOTTER  
  float L,R,U,V,dy,dx;

  Serial.print(x);  Serial.print(' ');
  Serial.print(y);  Serial.print(' ');
  Serial.print(L);  Serial.print(' ');
  Serial.print(R);  Serial.print(' ');
  Serial.print(U);  Serial.print(' ');
  Serial.print(V);  Serial.print('\n');
#endif
}


/** 
 * Forward Kinematics - turns L1,L2 lengths into XY coordinates
 * @param motorStepArray a measure of each belt to that plotter position
 * @param x the resulting cartesian coordinate
 * @param y the resulting cartesian coordinate
 */
void FK(long *motorStepArray,float &x,float &y) {
#ifdef COREXY
  float a = motorStepArray[0] * threadPerStep;
  float b = motorStepArray[1] * threadPerStep;

  x = (float)( a + b ) / 2.0;
  y = x - (float)b;
#endif
#ifdef TRADITIONALXY
  x = motorStepArray[0] * threadPerStep;
  y = motorStepArray[1] * threadPerStep;
#endif
#if defined(POLARGRAPH2) || defined(ZARPLOTTER)
  // use law of cosines: theta = acos((a*a+b*b-c*c)/(2*a*b));
  float a = (float)motorStepArray[0] * threadPerStep;
  float b = (limit_xmax-limit_xmin);
  float c = (float)motorStepArray[1] * threadPerStep;

  // slow, uses trig
  // we know law of cosines:   cc = aa + bb -2ab * cos( theta )
  // or cc - aa - bb = -2ab * cos( theta )
  // or ( aa + bb - cc ) / ( 2ab ) = cos( theta );
  // or theta = acos((aa+bb-cc)/(2ab));
  //x = cos(theta)*l1 + limit_xmin;
  //y = sin(theta)*l1 + limit_ymax;
  // and we know that cos(acos(i)) = i
  // and we know that sin(acos(i)) = sqrt(1-i*i)
  float theta = ((a*a+b*b-c*c)/(2.0*a*b));
  x = theta * a + limit_xmin;
  y = limit_ymax - (sqrt( 1.0 - theta * theta ) * a);
#endif
}


/**
 * 
 */
void processConfig() {
  float newT = parseNumber('T',limit_ymax);
  float newB = parseNumber('B',limit_ymin);
  float newR = parseNumber('R',limit_xmax);
  float newL = parseNumber('L',limit_xmin);
  // @TODO: check t>b, r>l ?
  adjustDimensions(newT,newB,newR,newL);

  // invert motor direction
  char i=parseNumber('I',m1i);
  char j=parseNumber('J',m2i);
  char m=parseNumber('U',m4i);
  char n=parseNumber('V',m5i);
  adjustInversions(i,j,m,n);
  
  printConfig();
  teleport(posx,posy);
}


/**
 * @TODO: remove this bullshit and make users flip their motor cable themselves.
 */
void adjustInversions(int m1,int m2,int m4,int m5) {
  //Serial.print(F("Adjusting inversions to "));

  if(m1>0) {
    motors[0].reel_in  = HIGH;
    motors[0].reel_out = LOW;
  } else if(m1<0) {
    motors[0].reel_in  = LOW;
    motors[0].reel_out = HIGH;
  }

  if(m2>0) {
    motors[1].reel_in  = HIGH;
    motors[1].reel_out = LOW;
  } else if(m2<0) {
    motors[1].reel_in  = LOW;
    motors[1].reel_out = HIGH;
  }

#if NUM_MOTORS>=4
  if(m4>0) {
    motors[3].reel_in  = HIGH;
    motors[3].reel_out = LOW;
  } else if(m4<0) {
    motors[3].reel_in  = LOW;
    motors[3].reel_out = HIGH;
  }
#endif
#if NUM_MOTORS>=5
  if(m5>0) {
    motors[4].reel_in  = HIGH;
    motors[4].reel_out = LOW;
  } else if(m5<0) {
    motors[4].reel_in  = LOW;
    motors[4].reel_out = HIGH;
  }
#endif

  if( m1!=m1i || m2 != m2i
#if NUM_MOTORS>=4
|| m4 != m4i
#endif
#if NUM_MOTORS>=5
|| m5 != m5i
#endif
  ) {
    // loadInversions() should never reach this point in the code.
    m1i=m1;
    m2i=m2;
#if NUM_MOTORS>=4
    m4i=m4;
#endif
#if NUM_MOTORS>=5
    m5i=m5;
#endif
    saveInversions();
  }
}


/**
 * Test that IK(FK(A))=A
 */
void testKinematics() {
  long A[NUM_MOTORS],i,j;
  float C,D,x=0,y=0;

  for(i=0;i<3000;++i) {
    x = random(limit_xmax,limit_xmax)*0.1;
    y = random(limit_ymin,limit_ymax)*0.1;

    IK(x,y,A);
    FK(A,C,D);
    Serial.print(F("\tx="));  Serial.print(x);
    Serial.print(F("\ty="));  Serial.print(y);
    for(int j=0;j<NUM_MOTORS;++j) {
      Serial.print('\t');
      Serial.print(AxisLetters[j]);
      Serial.print(A[j]);
    }
    Serial.print(F("\tx'="));  Serial.print(C);
    Serial.print(F("\ty'="));  Serial.print(D);
    Serial.print(F("\tdx="));  Serial.print(C-x);
    Serial.print(F("\tdy="));  Serial.println(D-y);
  }
}

/**
 * Translate the XYZ through the IK to get the number of motor steps and move the motors.
 * @input x destination x value
 * @input y destination y value
 * @input z destination z value
 * @input new_feed_rate speed to travel along arc
 */
void polargraph_line(float x,float y,float z,float new_feed_rate) {
  long steps[NUM_MOTORS];
  IK(x,y,steps);
  posx=x;
  posy=y;
  posz=z;

  steps[2]=z;

  feed_rate = new_feed_rate;
  motor_line(steps,new_feed_rate);
}


/**
 * Move the pen holder in a straight line using bresenham's algorithm
 * @input x destination x value
 * @input y destination y value
 * @input z destination z value
 * @input new_feed_rate speed to travel along arc
 */
void line_safe(float x,float y,float z,float new_feed_rate) {
  x-=tool_offset[current_tool].x;
  y-=tool_offset[current_tool].y;
  z-=tool_offset[current_tool].z;

  // split up long lines to make them straighter?
  Vector3 destination(x,y,z);
  Vector3 startPoint(posx,posy,posz);
  Vector3 dp = destination - startPoint;
  Vector3 temp;

  float len=dp.Length();
  int pieces = ceil(dp.Length() * (float)SEGMENT_PER_CM_LINE );

  float a;
  long j;

  // draw everything up to (but not including) the destination.
  for(j=1;j<pieces;++j) {
    a=(float)j/(float)pieces;
    temp = dp * a + startPoint;
    polargraph_line(temp.x,temp.y,temp.z,new_feed_rate);
  }
  // guarantee we stop exactly at the destination (no rounding errors).
  polargraph_line(x,y,z,new_feed_rate);
}


/**
 * This method assumes the limits have already been checked.
 * This method assumes the start and end radius match.
 * This method assumes arcs are not >180 degrees (PI radians)
 * @input cx center of circle x value
 * @input cy center of circle y value
 * @input x destination x value
 * @input y destination y value
 * @input z destination z value
 * @input dir - ARC_CW or ARC_CCW to control direction of arc
 * @input new_feed_rate speed to travel along arc
 */
void arc(float cx,float cy,float x,float y,float z,char clockwise,float new_feed_rate) {
  // get radius
  float dx = posx - cx;
  float dy = posy - cy;
  float sr=sqrt(dx*dx+dy*dy);

  // find angle of arc (sweep)
  float sa=atan3(dy,dx);
  float ea=atan3(y-cy,x-cx);
  float er=sqrt(dx*dx+dy*dy);
  
  float da=ea-sa;
  if(clockwise!=0 && da<0) ea+=2*PI;
  else if(clockwise==0 && da>0) sa+=2*PI;
  da=ea-sa;
  float dr = er-sr;

  // get length of arc
  // float circ=PI*2.0*radius;
  // float len=theta*circ/(PI*2.0);
  // simplifies to
  float len1 = abs(da) * sr;
  float len = sqrt( len1 * len1 + dr * dr );

  int i, segments = ceil( len * SEGMENT_PER_CM_ARC );

  float nx, ny, nz, angle3, scale;
  float a,r;
  for(i=0;i<=segments;++i) {
    // interpolate around the arc
    scale = ((float)i)/((float)segments);

    a = ( da * scale ) + sa;
    r = ( dr * scale ) + sr;
    
    nx = cx + cos(a) * r;
    ny = cy + sin(a) * r;
    nz = ( z - posz ) * scale + posz;
    // send it to the planner
    line_safe(nx,ny,nz,new_feed_rate);
  }
}


/**
 * Instantly move the virtual plotter position.  Does not check if the move is valid.
 */
void teleport(float x,float y) {
  wait_for_empty_segment_buffer();
  
  posx=x;
  posy=y;

  // @TODO: posz?
  long steps[NUM_MOTORS];
  IK(posx,posy,steps);

  motor_set_step_count(steps);
}


/**
 * Print a helpful message to serial.  The first line must never be changed to play nice with the JAVA software.
 */
void help() {
  Serial.print(F("\n\nHELLO WORLD! I AM DRAWBOT #"));
  Serial.println(robot_uid);
  sayVersionNumber();
  Serial.println(F("== http://www.makelangelo.com/ =="));
  Serial.println(F("M100 - display this message"));
  Serial.println(F("M101 [Tx.xx] [Bx.xx] [Rx.xx] [Lx.xx]"));
  Serial.println(F("       - display/update board dimensions."));
  Serial.println(F("As well as the following G-codes (http://en.wikipedia.org/wiki/G-code):"));
  Serial.println(F("G00,G01,G02,G03,G04,G28,G90,G91,G92,M18,M114"));
}


void sayVersionNumber() {
  char versionNumber = loadVersion();
  
  Serial.print(F("Firmware v"));
  Serial.println(versionNumber,DEC);
}

/**
 * Starting from the home position, bump the switches and measure the length of each belt.
 * Does not save the values, only reports them to serial.
 */
void calibrateBelts() {
#ifndef ZARPLOTTER
#ifdef USE_LIMIT_SWITCH
  wait_for_empty_segment_buffer();

  Serial.println(F("Find switches..."));
  
  // reel in the left motor and the right motor out until contact is made.
  digitalWrite(MOTOR_0_DIR_PIN,motors[0].reel_out);
  digitalWrite(MOTOR_1_DIR_PIN,motors[1].reel_out);
  int left=0, right=0;
  #ifdef ZARPLOTTER
  digitalWrite(MOTOR_2_DIR_PIN,motors[2].reel_out);
  digitalWrite(MOTOR_3_DIR_PIN,motors[3].reel_out);
  int bLeft=0,bRight=0;
  #endif
  long steps[NUM_MOTORS];

  IK(homeX,homeY,steps);
  findStepDelay();

  do {
    if(left==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_LEFT )==LOW ) {
        // switch hit
        left=1;
      }
      // switch not hit yet, keep moving
      digitalWrite(MOTOR_0_STEP_PIN,HIGH);
      digitalWrite(MOTOR_0_STEP_PIN,LOW);
      steps[0]++;
    }
    if(right==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_RIGHT )==LOW ) {
        // switch hit
        right=1;
      }
      // switch not hit yet, keep moving
      digitalWrite(MOTOR_1_STEP_PIN,HIGH);
      digitalWrite(MOTOR_1_STEP_PIN,LOW);
      steps[1]++;
    }
    #ifdef ZARPLOTTER
    if(bLeft==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_LEFT2 )==LOW ) {
        // switch hit
        bLeft=1;
      }
      // switch not hit yet, keep moving
      digitalWrite(MOTOR_2_STEP_PIN,HIGH);
      digitalWrite(MOTOR_2_STEP_PIN,LOW);
      steps[2]++;
    }
    if(bRight==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_RIGHT2 )==LOW ) {
        // switch hit
        bRight=1;
      }
      // switch not hit yet, keep moving
      digitalWrite(MOTOR_3_STEP_PIN,HIGH);
      digitalWrite(MOTOR_3_STEP_PIN,LOW);
      steps[3]++;
    }
    #endif
    pause(step_delay);
  } while(left+right
  #ifdef ZARPLOTTER
  +bLeft+bRight
  #endif
  <NUM_MOTORS);

  // make sure there's no momentum to skip the belt on the pulley.
  delay(500);
  
  Serial.println(F("Estimating position..."));
  calibrateLeft  = (float)steps[0] * threadPerStep;
  calibrateRight = (float)steps[1] * threadPerStep;
  
  reportCalibration();
  
  // current position is...
  float x,y;
  FK(steps,x,y);
  teleport(x,y);
  where();

  // go home.
  Serial.println(F("Homing..."));

  Vector3 offset=get_end_plus_offset();
  line_safe(homeX,homeY,offset.z,feed_rate);
  Serial.println(F("Done."));
#endif // USE_LIMIT_SWITCH
#endif // ZARPLOTTER
}


void recordHome() {
#ifndef ZARPLOTTER
#ifdef USE_LIMIT_SWITCH
  wait_for_empty_segment_buffer();

  Serial.println(F("Record home..."));

  digitalWrite(MOTOR_0_DIR_PIN,motors[0].reel_out);
  digitalWrite(MOTOR_1_DIR_PIN,motors[1].reel_out);
  int left=0;
  int right=0;
  long count[NUM_MOTORS];
  
  // we start at home position, so we know (x,y)->(left,right) value here.
  IK(homeX,homeY,count);
  Serial.print(F("HX="));  Serial.println(homeX);
  Serial.print(F("HY="));  Serial.println(homeY);
  //Serial.print(F("L1="));  Serial.println(leftCount);
  //Serial.print(F("R1="));  Serial.println(rightCount);
  
  do {
    if(left==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_LEFT)==LOW ) {
        left=1;
        Serial.println(F("Left..."));
      }
      ++count[0];
      digitalWrite(MOTOR_0_STEP_PIN,HIGH);
      digitalWrite(MOTOR_0_STEP_PIN,LOW);
    }
    if(right==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_RIGHT)==LOW ) {
        right=1;
        Serial.println(F("Right..."));
      }
      ++count[1];
      digitalWrite(MOTOR_1_STEP_PIN,HIGH);
      digitalWrite(MOTOR_1_STEP_PIN,LOW);
    }
    pause(step_delay*2);
  } while(left+right<2);

  calibrateLeft =count[0];
  calibrateRight=count[1];
  
  // now we have the count from home position to switches.  record that value.
  saveCalibration();
  reportCalibration();
  
  // current position is...
  float x,y;
  FK(count,x,y);
  teleport(x,y);
  where();

  // go home.
  Serial.println(F("Homing..."));

  Vector3 offset=get_end_plus_offset();
  line_safe(homeX,homeY,offset.z,feed_rate);
  Serial.println(F("Done."));
#endif // USER_LIMIT_SWITCH
#endif // ZARPLOTTER
}


/**
 * If limit switches are installed, move to touch each switch so that the pen holder can move to home position.
 */
void findHome() {
#ifndef ZARPLOTTER
#ifdef USE_LIMIT_SWITCH
  wait_for_empty_segment_buffer();
  
  Serial.println(F("Find Home..."));

  findStepDelay();

  // reel in the left motor and the right motor out until contact is made.
  digitalWrite(MOTOR_0_DIR_PIN,motors[0].reel_out);
  digitalWrite(MOTOR_1_DIR_PIN,motors[1].reel_out);
  int left=0, right=0;
  do {
    if(left==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_LEFT)==LOW ) {
        left=1;
        Serial.println(F("Left..."));
      }
      digitalWrite(MOTOR_0_STEP_PIN,HIGH);
      digitalWrite(MOTOR_0_STEP_PIN,LOW);
    }
    if(right==0) {
      if( digitalRead(LIMIT_SWITCH_PIN_RIGHT)==LOW ) {
        right=1;
        Serial.println(F("Right..."));
      }
      digitalWrite(MOTOR_1_STEP_PIN,HIGH);
      digitalWrite(MOTOR_1_STEP_PIN,LOW);
    }
    pause(step_delay);
  } while(left+right<2);

  // make sure there's no momentum to skip the belt on the pulley.
  delay(500);
  
  Serial.println(F("Estimating position..."));
  long count[NUM_MOTORS];
  count[0] = calibrateLeft;
  count[1] = calibrateRight;
  Serial.print("cl=");   Serial.println(calibrateLeft);
  Serial.print("cr=");   Serial.println(calibrateRight);
  Serial.print("t=");    Serial.println(threadPerStep);
  
  // current position is...
  float x,y;
  FK(count,x,y);
  teleport(x,y);
  where();

  // go home.
  Serial.println(F("Homing..."));

  Vector3 offset=get_end_plus_offset();
  line_safe(homeX,homeY,offset.z,feed_rate);
  Serial.println(F("Done."));
#endif // USER_LIMIT_SWITCH
#endif // ZARPLOTTER
}


/**
 * Print the X,Y,Z, feedrate, and acceleration to serial.
 * Equivalent to gcode M114
 */
void where() {
  wait_for_empty_segment_buffer();

  Serial.print(F("X"   ));  Serial.print(posx);
  Serial.print(F(" Y"  ));  Serial.print(posy);
  Serial.print(F(" Z"  ));  Serial.print(posz);
  Serial.print(' '      );  printFeedRate();
  Serial.print(F(" A"  ));  Serial.println(acceleration);
  Serial.print(F(" HX="));  Serial.print(homeX);
  Serial.print(F(" HY="));  Serial.println(homeY);
}


/**
 * Print the machine limits to serial.
 */
void printConfig() {
  Serial.print(F("("));      Serial.print(limit_xmin);
  Serial.print(F(","));      Serial.print(limit_ymax);
  Serial.print(F(") - ("));  Serial.print(limit_xmax);
  Serial.print(F(","));      Serial.print(limit_ymin);
  Serial.print(F(")\n"));

  Serial.print('I');  Serial.print(m1i,DEC);
  Serial.print(" J");  Serial.print(m2i,DEC);
#if NUM_MOTORS>=4
  Serial.print(" U");  Serial.print(m4i,DEC);
#endif
#if NUM_MOTORS>=5
  Serial.print(" V");  Serial.print(m5i,DEC);
#endif
  Serial.println();
}


/**
 * Set the relative tool offset
 * @input axis the active tool id
 * @input x the x offset
 * @input y the y offset
 * @input z the z offset
 */
void set_tool_offset(int axis,float x,float y,float z) {
  tool_offset[axis].x=x;
  tool_offset[axis].y=y;
  tool_offset[axis].z=z;
}


/**
 * @return the position + active tool offset
 */
Vector3 get_end_plus_offset() {
  return Vector3(tool_offset[current_tool].x + posx,
                 tool_offset[current_tool].y + posy,
                 tool_offset[current_tool].z + posz);
}


/**
 * Change the currently active tool
 */
void tool_change(int tool_id) {
  if(tool_id < 0) tool_id=0;
  if(tool_id >= NUM_TOOLS) tool_id=NUM_TOOLS-1;
  current_tool=tool_id;
#ifdef HAS_SD
  if(sd_printing_now) {
    sd_printing_paused=true;
  }
#endif
}


/**
 * Look for character /code/ in the buffer and read the float that immediately follows it.
 * @return the value found.  If nothing is found, /val/ is returned.
 * @input code the character to look for.
 * @input val the return value if /code/ is not found.
 **/
float parseNumber(char code,float val) {
  char *ptr=serialBuffer;  // start at the beginning of buffer
  while((long)ptr > 1 && (*ptr) && (long)ptr < (long)serialBuffer+sofar) {  // walk to the end
    if(*ptr==code) {  // if you find code on your walk,
      return atof(ptr+1);  // convert the digits that follow into a float and return it
    }
    ptr=strchr(ptr,' ')+1;  // take a step from here to the letter after the next space
  }
  return val;  // end reached, nothing found, return default val.
}


/**
 * process commands in the serial receive buffer
 */
void processCommand() {
  // blank lines
  if(serialBuffer[0]==';') return;

  long cmd;

  // is there a line number?
  cmd=parseNumber('N',-1);
  if(cmd!=-1 && serialBuffer[0]=='N') {  // line number must appear first on the line
    if( cmd != line_number ) {
      // wrong line number error
      Serial.print(F("BADLINENUM "));
      Serial.println(line_number);
      return;
    }

    // is there a checksum?
    if(strchr(serialBuffer,'*')!=0) {
      // yes.  is it valid?
      char checksum=0;
      int c=0;
      while(serialBuffer[c]!='*' && c<MAX_BUF) checksum ^= serialBuffer[c++];
      c++; // skip *
      int against = strtod(serialBuffer+c,NULL);
      if( checksum != against ) {
        Serial.print(F("BADCHECKSUM "));
        Serial.println(line_number);
        return;
      }
    } else {
      Serial.print(F("NOCHECKSUM "));
      Serial.println(line_number);
      return;
    }

    line_number++;
  }

  if(!strncmp(serialBuffer,"UID",3)) {
    robot_uid=atoi(strchr(serialBuffer,' ')+1);
    saveUID();
  }


  cmd=parseNumber('M',-1);
  switch(cmd) {
  case 6:  tool_change(parseNumber('T',current_tool));  break;
  case 17:  motor_engage();  break;
  case 18:  motor_disengage();  break;
  case 100:  help();  break;
  case 101:  processConfig();  break;
  case 102:  printConfig();  break;
  case 110:  line_number = parseNumber('N',line_number);  break;
  case 114:  where();  break;
  default:  break;
  }

  cmd=parseNumber('G',-1);
  switch(cmd) {
  case 0:
  case 1: {  // line
      Vector3 offset=get_end_plus_offset();
      acceleration = min(max(parseNumber('A',acceleration),1),2000);
      line_safe( parseNumber('X',(absolute_mode?offset.x:0)*10)*0.1 + (absolute_mode?0:offset.x),
                 parseNumber('Y',(absolute_mode?offset.y:0)*10)*0.1 + (absolute_mode?0:offset.y),
                 parseNumber('Z',(absolute_mode?offset.z:0)   )     + (absolute_mode?0:offset.z),
                 parseNumber('F',feed_rate) );
      break;
    }
  case 2:
  case 3: {  // arc
      Vector3 offset=get_end_plus_offset();
      acceleration = min(max(parseNumber('A',acceleration),1),2000);
      setFeedRate(parseNumber('F',feed_rate));
      arc(parseNumber('I',(absolute_mode?offset.x:0)*10)*0.1 + (absolute_mode?0:offset.x),
          parseNumber('J',(absolute_mode?offset.y:0)*10)*0.1 + (absolute_mode?0:offset.y),
          parseNumber('X',(absolute_mode?offset.x:0)*10)*0.1 + (absolute_mode?0:offset.x),
          parseNumber('Y',(absolute_mode?offset.y:0)*10)*0.1 + (absolute_mode?0:offset.y),
          parseNumber('Z',(absolute_mode?offset.z:0)) + (absolute_mode?0:offset.z),
          (cmd==2) ? 1 : 0,
          parseNumber('F',feed_rate) );
      break;
    }
  case 4:  {  // dwell
      wait_for_empty_segment_buffer();
      float delayTime = parseNumber('S',0) + parseNumber('P',0)*1000.0f;
      pause(delayTime);
      break;
    }
  case 28:  findHome();  break;
  case 29:  calibrateBelts();  break;
  case 54:
  case 55:
  case 56:
  case 57:
  case 58:
  case 59: {  // 54-59 tool offsets
    int tool_id=cmd-54;
    set_tool_offset(tool_id,parseNumber('X',tool_offset[tool_id].x),
                            parseNumber('Y',tool_offset[tool_id].y),
                            parseNumber('Z',tool_offset[tool_id].z));
    break;
    }
  case 90:  absolute_mode=1;  break;  // absolute mode
  case 91:  absolute_mode=0;  break;  // relative mode
  case 92: {  // set position (teleport)
      Vector3 offset = get_end_plus_offset();
      teleport( parseNumber('X',(absolute_mode?offset.x:0)*10)*0.1 + (absolute_mode?0:offset.x),
                 parseNumber('Y',(absolute_mode?offset.y:0)*10)*0.1 + (absolute_mode?0:offset.y)
               //parseNumber('Z',(absolute_mode?offset.z:0)) + (absolute_mode?0:offset.z)
                 );
      break;
    }
  default:  break;
  }

  cmd=parseNumber('D',-1);
  switch(cmd) {
  case 0:  jogMotors();  break;
  case 1: {
      // adjust spool diameters
      float amountL=parseNumber('L',pulleyDiameter);

      float d1=pulleyDiameter;
      adjustPulleyDiameter(amountL);
      if(pulleyDiameter != d1) {
        // Update EEPROM
        savePulleyDiameter();
      }
    }
    break;
  case 2:
    Serial.print(F("D2 D"));
    Serial.print(pulleyDiameter);
    Serial.print(F("TPS"));
    Serial.println(threadPerStep*100.0f);
    break;
//  case 3:  SD_ListFiles();  break;
  case 4:  SD_StartPrintingFile(strchr(serialBuffer,' ')+1);  break;  // read file
  case 5:
    sayVersionNumber();
  case 6:  // set home
    setHome(parseNumber('X',(absolute_mode?homeX:0)*10)*0.1 + (absolute_mode?0:homeX),
            parseNumber('Y',(absolute_mode?homeY:0)*10)*0.1 + (absolute_mode?0:homeY));
  case 7:  // set calibration length
      calibrateLeft = parseNumber('L',calibrateLeft);
      calibrateRight = parseNumber('R',calibrateRight);
      #ifdef ZARPLOTTER
      calibrateBLeft = parseNumber('U',calibrateBLeft);
      calibrateBRight = parseNumber('V',calibrateBRight);
      #endif
      // fall through to case 8, report calibration.
      //reportCalibration();
    //break;
  case 8:  reportCalibration();  break;
  case 9:  // save calibration length
    saveCalibration();
    break;
  case 10:  // get hardware version
    Serial.print(F("D10 V"));
    Serial.println(MAKELANGELO_HARDWARE_VERSION);
    break;
  case 11:
    // if you accidentally upload m3 firmware to an m5 then upload it ONCE with this line uncommented.
    adjustDimensions(50,-50,-32.5,32.5);
    adjustInversions(1,-1,1,-1);
    adjustPulleyDiameter(4.0/PI);
    savePulleyDiameter();
    saveCalibration();
    break;
  case 12: recordHome();
  default:  break;
  }
}


void jogMotors() {
  int i,j,amount;
  
  findStepDelay();
  for(i=0;i<NUM_MOTORS;++i) {
    if(motorNames[i]==0) continue;
    amount = parseNumber(motorNames[i],0);
    if(amount!=0) {
      Serial.print(F("Moving "));
      Serial.print(motorNames[i]);
      Serial.print(F(" ("));
      Serial.print(i);
      Serial.print(F(") "));
      Serial.print(amount);
      Serial.println(F(" steps."));
      digitalWrite(motors[i].dir_pin,amount < 0 ? motors[i].reel_in : motors[i].reel_out);
      amount=abs(amount);
      for(j=0;j<amount;++j) {
        digitalWrite(motors[i].step_pin,HIGH);
        digitalWrite(motors[i].step_pin,LOW);
        pause(step_delay);
      }
    }
  }
}


void reportCalibration() {
  Serial.print(F("D8 L"));
  Serial.print(calibrateLeft);
  Serial.print(F(" R"));
  Serial.println(calibrateRight);
#ifdef ZARPLOTTER
  Serial.print(F(" U"));
  Serial.println(calibrateBLeft);
  Serial.print(F(" V"));
  Serial.println(calibrateBRight);
#endif
}


// equal to three decimal places?
boolean equalEpsilon(float a,float b) {
  int aa = a*10;
  int bb = b*10;
  //Serial.print("aa=");        Serial.print(aa);
  //Serial.print("\tbb=");      Serial.print(bb);
  //Serial.print("\taa==bb ");  Serial.println(aa==bb?"yes":"no");
  
  return aa==bb;
}


void setHome(float x,float y) {
  boolean dx = equalEpsilon(x,homeX);
  boolean dy = equalEpsilon(y,homeY);
  if( dx==false || dy==false ) {
    //Serial.print(F("Was    "));    Serial.print(homeX);    Serial.print(',');    Serial.println(homeY);
    //Serial.print(F("Is now "));    Serial.print(    x);    Serial.print(',');    Serial.println(    y);
    //Serial.print(F("DX="));    Serial.println(dx?"true":"false");
    //Serial.print(F("DY="));    Serial.println(dy?"true":"false");
    homeX = x;
    homeY = y;
    saveHome();
  }
}


/**
 * prepares the input buffer to receive a new message and tells the serial connected device it is ready for more.
 */
void parser_ready() {
  sofar=0;  // clear input buffer
  Serial.print(F("\n> "));  // signal ready to receive input
  last_cmd_time = millis();
}


/**
 * reset all tool offsets
 */
void tools_setup() {
  for(int i=0;i<NUM_TOOLS;++i) {
    set_tool_offset(i,0,0,0);
  }
}


/**
 * runs once on machine start
 */
void setup() {
  // start communications
  Serial.begin(BAUD);

  loadConfig();

  motor_setup();
  motor_engage();
  tools_setup();

  //easyPWM_init();
  SD_init();
  LCD_init();

  // initialize the plotter position.
  teleport(0,0);
  setPenAngle(PEN_UP_ANGLE);
  setFeedRate(DEFAULT_FEEDRATE);
  
  // display the help at startup.
  help();
  
  parser_ready();
}


/**
 * See: http://www.marginallyclever.com/2011/10/controlling-your-arduino-through-the-serial-monitor/
 */
void Serial_listen() {
  // listen for serial commands
  while(Serial.available() > 0) {
    char c = Serial.read();
    if(c=='\r') continue;
    if(sofar<MAX_BUF) serialBuffer[sofar++]=c;
    if(c=='\n') {
      serialBuffer[sofar]=0;

      // echo confirmation
//      Serial.println(F(serialBuffer));

      // do something with the command
      processCommand();
      parser_ready();
    }
  }
}


/**
 * main loop
 */
void loop() {
  Serial_listen();
  SD_check();
#ifdef HAS_LCD
  LCD_update();
#endif

  // The PC will wait forever for the ready signal.
  // if Arduino hasn't received a new instruction in a while, send ready() again
  // just in case USB garbled ready and each half is waiting on the other.
  if( !segment_buffer_full() && (millis() - last_cmd_time) > TIMEOUT_OK ) {
    parser_ready();
  }
}


/**
 * This file is part of makelangelo-firmware.
 *
 * makelangelo-firmware is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * makelangelo-firmware is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DrawbotGUI.  If not, see <http://www.gnu.org/licenses/>.
 */
