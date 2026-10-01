// *****************************************************************************
// *****************************************************************************
// Copyright 2013-2020, Cadence Design Systems
// 
// This  file  is  part  of  the  Cadence  LEF/DEF  Open   Source
// Distribution,  Product Version 5.8. 
// 
// Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
// 
//        http://www.apache.org/licenses/LICENSE-2.0
// 
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
//    implied. See the License for the specific language governing
//    permissions and limitations under the License.
// 
// For updates, support, or to become part of the LEF/DEF Community,
// check www.openeda.org for details.
// 
//  $Author: arakhman $
//  $Revision: #6 $
//  $Date: 2013/08/09 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "defrData.hpp"
#include "defrSettings.hpp"

using namespace std;

BEGIN_LEFDEF_PARSER_NAMESPACE

defrData::defrData(const defrCallbacks *pCallbacks,
                   const defrSettings  *pSettings,
                   defrSession         *pSession)
: callbacks(pCallbacks),
  settings(pSettings),
  session(pSession),
  Subnet(0),
  aOxide(0),
  assertionWarnings(0),
  bit_is_keyword(0),
  bitsNum(0),
  blockageWarnings(0),
  by_is_keyword(0),
  caseSensitiveWarnings(0),
  componentWarnings(0),
  constraintWarnings(0),
  cover_is_keyword(0),
  defIgnoreVersion(0),
  defInvalidChar(0),
  defMsg(NULL),
  defMsgCnt(5500),
  defMsgPrinted(0),
  defPrintTokens(0),
  defPropDefType('\0'),
  defRetVal(0),
  def_warnings(0),
  defaultCapWarnings(0),
  defrLog(0),
  do_is_keyword(0),
  dumb_mode(0),
  errors(0),
  fillWarnings(0),
  first_buffer(0),
  fixed_is_keyword(0),
  gcellGridWarnings(0),
  hasBlkLayerComp(0),
  hasBlkLayerSpac(0),
  hasBlkLayerTypeComp(0),
  hasBlkPlaceComp(0),
  hasBlkPlaceTypeComp(0),
  hasDef60BlkPlaceTypeComp(0),
  hasBusBit(0),
  hasDes(0),
  hasDivChar(0),
  hasDoStep(0),
  hasNameCase(0),
  hasOpenedDefLogFile(0),
  hasPort(0),
  hadPortOnce(0),
  hasVer(0),
  hasFatalError(0),
  iOTimingWarnings(0),
  input_level(-1),
  last(NULL),
  magic((char*)malloc(1)),
  mask_is_keyword(0),
  mustjoin_is_keyword(0),
  names_case_sensitive(1),
  needNPCbk(0),
  needSNPCbk(0),
  netOsnet(0),
  netWarnings(0),
  new_is_keyword(0),
  next(NULL),
  nlines(1),
  nl_token(FALSE),
  no_num(0),
  nonDefaultWarnings(0),
  nondef_is_keyword(0),
  ntokens(0),
  orient_is_keyword(0),
  width_is_keyword(0),
  pinExtWarnings(0),
  pinWarnings(0),
  real_num(0),
  rect_is_keyword(0),
  regTypeDef(0),
  regionWarnings(0),
  ringPlace(0),
  routed_is_keyword(0),
  rowName(NULL),
  rowWarnings(0),
  sNetWarnings(0),
  save_x(0.0),
  save_y(0.0),
  scanchainWarnings(0),
  shieldName(NULL),
  shiftBuf(0),
  shiftBufLength(0),
  specialWire_mask(0),
  step_is_keyword(0),
  stylesWarnings(0),
  trackWarnings(0),
  unitsWarnings(0),
  versionWarnings(0),
  viaRule(0),
  viaWarnings(0),
  virtual_is_keyword(0),
  deftokenLength(TOKEN_SIZE),
  warningMsg(NULL),
  routeStatus((char*) "ROUTED"),
  shapeType(NULL),
  VersionNum(5.8),
  xStep(0),
  yStep(0),
  lVal(0.0),
  rVal(0.0),
  // defrReader vars
  Shield(NULL),
  Wire(NULL),
  PathObj(NULL),
  Prop(this),
  Site(this),
  ComponentMaskShiftLayer(this),
  PinCap(),
  CannotOccupy(this),
  Canplace(this),
  DieArea(),
  Pin(this),
  Row(this),
  Track(this),
  GcellGrid(this),
  Via(this),
  Region(this),
  Group(this),
  Assertion(this),
  Scanchain(this),
  IOTiming(this),
  FPC(this),
  TimingDisable(this),
  Partition(this),
  PinProp(this),
  Blockage(this),
  Slot(this),
  Fill(this),
  NonDefault(this),
  Styles(),
  Geometries(this),
  doneDesign(0),
  deftoken((char*)malloc(TOKEN_SIZE)),
  uc_token((char*)malloc(TOKEN_SIZE)),
  pv_deftoken((char*)malloc(TOKEN_SIZE)),
  File(0),
  props(new defrProps())
{
    magic[0] = '\0';
    deftoken[0] = '\0';
    History_text.push_back('\0');

    memset(msgLimit, 0, DEF_MSGS * sizeof(int));
    memset(buffer, 0, IN_BUF_SIZE * sizeof(char));
    memset(ring, 0, RING_SIZE * sizeof(char*));
    memset(ringSizes, 0, RING_SIZE * sizeof(int));
    memset(lineBuffer, 0, MSG_SIZE * sizeof(char));

    // initRingBuffer
    int i;
    ringPlace = 0;
    for (i = 0; i < RING_SIZE; i++) {
        ring[i] = (char*)malloc(TOKEN_SIZE);
        ringSizes[i] = TOKEN_SIZE;
    }

    nlines = 1;
    last = buffer-1;
    next = buffer;
    first_buffer = 1;

    lVal = strtod("-2147483648", NULL);
    rVal = strtod("2147483647", NULL);
}


defrData::~defrData()
{
    // lex_un_init.
    /* Close the file */
    if (defrLog) {
        fclose(defrLog);
        defrLog = 0;
    }

    free(deftoken);
    free(uc_token);
    free(pv_deftoken);
    free(magic);

    // freeRingBuffer.
    int i;

    ringPlace = 0;
    for (i = 0; i < RING_SIZE; i++) {
        free(ring[i]);
    }

    delete props;
}

void 
defrData::defiError(int check, int msgNum, const char* mess) 
{
  /* check is 1 if the caller function has checked totalMsgLimit, etc. */

  if (!check) {
     if ((settings->totalDefMsgLimit > 0) && (defMsgPrinted >= settings->totalDefMsgLimit))
        return;
     if (settings->MsgLimit[msgNum-5000] > 0) {
        if (msgLimit[msgNum-5000] >= settings->MsgLimit[msgNum-5000])
           return;    /*over the limit*/
        msgLimit[msgNum-5000] = msgLimit[msgNum-5000] + 1;
     }
     defMsgPrinted++;
  }


  if (settings->ContextErrorLogFunction) {
      (*(settings->ContextErrorLogFunction))(session->UserData, mess);
  } else if (settings->ErrorLogFunction) {
    (*(settings->ErrorLogFunction))(mess);
  } else {
    fprintf(stderr, "%s", mess);
  }
}

void
defrData::startPath()
{
    PathObj->Init();
}

void
defrData::finishPath(int  reset,
                     int  *needCbk)
{
    if (Shield) {
        Shield->addPath(PathObj, reset, netOsnet, needCbk);
    } else {
        Wire->addPath(PathObj, reset, netOsnet, needCbk);
    }
}

const char* 
defrData::upperCase(const char* str) 
{
    const static char defiShift [] = {
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
      ' ',  '!',  '"',  '#',  '$',  '%',  '&', '\'', 
      '(',  ')',  '*',  '+',  ',',  '-',  '.',  '/', 
      '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7', 
      '8',  '9',  ':',  ';',  '<',  '=',  '>',  '?', 
      '@',  'A',  'B',  'C',  'D',  'E',  'F',  'G', 
      'H',  'I',  'J',  'K',  'L',  'M',  'N',  'O', 
      'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W', 
      'X',  'Y',  'Z',  '[', '\\',  ']',  '^',  '_', 
      '`',  'A',  'B',  'C',  'D',  'E',  'F',  'G', 
      'H',  'I',  'J',  'K',  'l',  'M',  'N',  'O', 
      'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W', 
      'X',  'Y',  'Z',  '{',  '|',  '}',  '~', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
     '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'
    };

  char* place = (char*)str;
  char* to;
  int len = (int) strlen(str) + 1;

  if (len > shiftBufLength) {
    if (shiftBuf == 0) {
      len = len < 64 ? 64 : len;
      shiftBuf = (char*)malloc(len);
      shiftBufLength = len;
    } else {
      free(shiftBuf);
      shiftBuf = (char*)malloc(len);
      shiftBufLength = len;
    }
  }

  to = shiftBuf;
  while (*place) {
      int i = (int)*place;
      place++;
    *to++ = defiShift[i];
  }
  *to = '\0';

  return shiftBuf;
}

int 
defrData::validateMaskInput(int input, int warningIndex, int getWarningsIndex) 
{
    if (VersionNum < 5.8 && input > 0) {
      if (warningIndex++ < getWarningsIndex) {
          defMsg = (char*)malloc(1000);
          sprintf (defMsg,
             "The MASK statement is available in version 5.8 and later.\nHowever, your DEF file is defined with version %.2f", VersionNum);
          defError(7415, defMsg);
          free(defMsg);
          if (checkErrors()) {
              return 1;
          }
          return 0;
        }
    }   
    
    return 1; 
}

int 
defrData::validateMaskShiftInput(const char* shiftMask, int warningIndex, int getWarningsIndex) 
{
    int shiftMaskLength = (int) strlen(shiftMask);
    int hasShiftData = 0;
    int hasError = 0;

    // Verification of the mask string
    for (int i = 0; i < shiftMaskLength; i++) {
        int curShift = shiftMask[i];
        
        if (curShift < '0' || curShift > '9') {
            hasError = 1;
        }

        if (curShift > '0') {
            hasShiftData = 1;
        }
    }

    if (hasError) {
        char *msg = (char*)malloc(1000);

        sprintf(msg, 
                "The MASKSHIFT value '%s' is not valid. The value should be a string consisting of decimal digits ('0' - '9').", 
                shiftMask);
        defError(7416, msg);
        free(msg);

        if (checkErrors()) {
            return 1;
        }

        return 0;
    }

    if (VersionNum < 5.8 && hasShiftData) {
        if (warningIndex++ < getWarningsIndex) {
            char *msg = (char*)malloc(1000);

            sprintf (msg, 
                     "The MASKSHIFT statement can be used only in DEF version 5.8 and later. This DEF file version is '%.2f'.", 
                     VersionNum);
            defError(7417, msg);
            free(msg);            
            if (checkErrors()) {
                return 1;
            }
        }
          
        return 0;
    }   
    
    return 1; 
}

double
defrData::convert_defname2num(char *versionName)
{
    char majorNm[80];
    char minorNm[80];
    char subMinorNm[80];

    double major = 0;
    double minor = 0;
    double subMinor = 0;

    double version;

    int num = sscanf(versionName, "%[^.].%[^.].%s", majorNm, minorNm, subMinorNm);

    if (num < 1) {
        return 0.0;
    }

    major = atof(majorNm);
    version = major;

    if (num >= 2) {
        minor = atof(minorNm);

        if (minor > 100)
            version += minor / 1000;
        else if (minor > 10)
            version += minor / 100;
        else
            version += minor / 10;
    }

    if (num > 2) {
        subMinor = atof(subMinorNm);
        version += subMinor / 1000;
    }

    return version;
}

int
defrData::numIsInt (char* volt) {
    if (strchr(volt, '.'))  // a floating point
       return 0;
    else
       return 1;
}

int 
defrData::defValidNum(int values) {
    char *outMsg;
    switch (values) {
        case 100:
        case 200:
        case 1000:
        case 2000:
                return 1;
        case 400:
        case 800:
        case 4000:
        case 8000:
        case 10000:
        case 20000:
             if (VersionNum < 5.6) {
                if (callbacks->UnitsCbk) {
                  if (unitsWarnings++ < settings->UnitsWarnings) {
                    outMsg = (char*)malloc(1000);
                    sprintf (outMsg,
                    "An error has been found while processing the DEF file '%s'\nUnit %d is a 5.6 or later syntax. Define the DEF file as 5.6 and then try again.",
                    session->FileName, values);
                    defError(6501, outMsg);
                    free(outMsg);
                  }
                }
                
                return 0;
             } else {
                return 1;
             }
    }
    if (callbacks->UnitsCbk) {
      if (unitsWarnings++ < settings->UnitsWarnings) {
        outMsg = (char*)malloc(10000);
        sprintf (outMsg,
          "The value %d defined for DEF UNITS DISTANCE MICRON is invalid\n. The valid values are 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, or 20000. Specify a valid value and then try again.", values);
        defError(6502, outMsg);
        free(outMsg);
        if (checkErrors()) {
            return 1;
        }
      }
    }
    return 0;
}

int 
defrData::def60NewSyntaxError(const char* address)
{
    if (nonDefaultWarnings++ < settings->NonDefaultWarnings) {
        defMsg = (char*)malloc(1000);
        sprintf(defMsg,
                "Syntax '%s' is available only in version 6.00 and later. However, your DEF file is defined with version %.2f.",
                address,
                VersionNum);
        defError(6568, defMsg);
        free(defMsg);
        return 1;
    }

    return 0;
}

int 
defrData::def60SyntaxError(const char* rule)
{
    if (nonDefaultWarnings++ < settings->NonDefaultWarnings) {
        defMsg = (char*)malloc(1000);
        sprintf(defMsg,
            "Syntax Error: In DEF version 6.00 and after %s. Translation will be terminated.",
            rule);
        defError(6570, defMsg);
        free(defMsg);
        return 1;
    }

    return 0;
}

int 
defrData::def60ObsoletedError(const char* address)
{
    if (nonDefaultWarnings++ < settings->NonDefaultWarnings) {
        defMsg = (char*)malloc(1000);
        sprintf(defMsg,
                "Syntax '%s' is obsoleted in version 6.00 and later. However, your DEF file is defined with version %.2f.",
                address,
                VersionNum);
        defError(6569, defMsg);
        free(defMsg);
        return 1;
    }

    return 0;
}


int defrData::def60ExclusiveStatementsError(const char* address, 
                                            const char* paramList)
{
    if (nonDefaultWarnings++ < settings->NonDefaultWarnings) {
        defMsg = (char*)malloc(1000);
        sprintf(defMsg,
                "Syntax Error: In '%s' statement %s statements are mutually exclusive, need to have only one parameter. Translation will be terminated.",
                address,
                paramList);
        defError(6571, defMsg);
        free(defMsg);
        return 1;
    }

    return 0;
}

int defrData::def60KeywordRequiresKeywordError(const char* addr,
                                               const char* keyword,
                                               const char* requiredKeywords)
{
    if (nonDefaultWarnings++ < settings->NonDefaultWarnings) {
        defMsg = (char*)malloc(1000);
        sprintf(defMsg,
                "Syntax Error: %s: '%s' requires: '%s'.",
                addr,
                keyword,
                requiredKeywords);
        defError(6572, defMsg);
        free(defMsg);
        return 1;
    }

    return 0;
}

void defrData::addProp(defiProp* prop)
{
    props->push_back(prop);
}

void defrData::cleanProps()
{
    delete props;
    props = new defrProps();
}

void defrData::setPropDataType(defiProp* prop, const char* propTypeName, defiPropType& propType)
{
    char    propTp = propType.propType(prop->propName());

    if (propTp == 'N') {
        warningMsg = (char*)malloc(strlen(prop->propName()) + strlen(propTypeName) + 40);
        sprintf(warningMsg,
            "The PropName %s is not defined for %s.",
            prop->propName(),
            propTypeName);
        defWarning(7010, warningMsg);
        free(warningMsg);
        prop->setPropNameMapString(prop->string());
    } else if (propTp == 'R') {
        // Need to add prop type, if found.
        prop->setPropReal();
    } 
}

void defrData::setPropsDataTypes(const char*    propTypeName, 
                                 defiPropType&  propType)
{
    for (size_t idx = 0; idx < props->size(); idx++) {
        defiProp* prop = (*props)[idx];

        setPropDataType(prop, propTypeName, propType);
    }
}

void defrData::addNetProps()
{
    for (size_t idx = 0; idx < props->size(); idx ++) {
        defiProp* prop = (*props)[idx];

        if (prop->hasNumber()) {
            Net->addNumProp(prop->propName(),
                           prop->number(),
                           prop->string(),
                           prop->dataType());
        } else {
            Net->addProp(prop->propName(), 
                        prop->string(), 
                        prop->dataType());
      }
    }
}

defrContext::defrContext(int ownConf)
: callbacks(0),
settings(0),
session(0),
data(0),
ownConfig(ownConf),
init_call_func(0)
{
}

defrContext defContext;

END_LEFDEF_PARSER_NAMESPACE
