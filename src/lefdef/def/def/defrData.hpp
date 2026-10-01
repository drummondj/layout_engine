// *****************************************************************************
// *****************************************************************************
// Copyright 2013 - 2020, Cadence Design Systems
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

#include <cstring>
#include <string>
#include <map>
#include <vector>

#include "defrReader.hpp"
#include "defrCallBacks.hpp"
#include "defrSettings.hpp"

#ifndef defrData_h
#define defrData_h

#define CURRENT_VERSION 6.0
#define RING_SIZE 10
#define IN_BUF_SIZE 16384
#define TOKEN_SIZE 4096
#define MSG_SIZE 100


BEGIN_LEFDEF_PARSER_NAMESPACE

class  defrString : public std::string
{
};

class defrStrings : public std::vector<std::string>
{
};

class defrInts : public std::vector<int>
{
};

class defrIntsInts : public std::vector<defrInts>
{
};

class  defrProps : public std::vector<defiProp*>
{
public:
    defrProps() {};
    ~defrProps() {
        for (size_t idx=0; idx < this->size(); idx++) {
            delete (*this)[idx];
        }
    }
};

class  defrPropsArray : public std::vector<defrProps*>
{
public:
    defrPropsArray() {};
    ~defrPropsArray() {
        for (size_t idx = 0; idx < this->size(); idx++) {
            delete (*this)[idx];
        }
    }
};


struct defCompareStrings 
{
    bool operator()(const std::string &lhs, const std::string &rhs) const {
        return std::strcmp(lhs.c_str(), rhs.c_str()) < 0;
    }
};

typedef std::map<std::string, std::string, defCompareStrings> defDefineMap;

typedef union {
        double dval ;
        int    integer ;
        char * string ;
        int    keyword ;  // really just a nop 
        struct defpoint pt;
        defiProp *prop;
        defTOKEN *tk;
} YYSTYPE;

#define YYSTYPE_IS_DECLARED

class defrData {

public:
    defrData(const defrCallbacks *pCallbacks,
             const defrSettings  *pSettings,
             defrSession         *pSession);
    ~defrData();

    inline int          defGetKeyword(const char* name, int *result);
    inline int          defGetDefine(const std::string &name, std::string &result);
    void                reload_buffer(); 
    int                 GETC();

    void                UNGETC(char ch);
    char*               ringCopy(const char* string);
    inline void         print_lines(long long lines);
    const char *        lines2str(long long lines);
    static inline void  IncCurPos(char **curPos, char **buffer, int *bufferSize);
    int                 DefGetToken(char **buffer, int *bufferSize);
    static void         uc_array(char *source, char *dest);
    int                 defyylex(YYSTYPE *pYylval);
    int                 sublex(YYSTYPE *pYylval);
    void                defError(int msgNum, const char *s);
    void                defyyerror(const char *s);
    void                defInfo(int msgNum, const char *s);
    void                defWarning(int msgNum, const char *s);

    void                defiError(int check, int msgNum, const char* mess);
    const char          *DEFCASE(const char* ch);
    void                startPath();
    void                finishPath(int  reset,
                                   int  *needCbk);
    const char          *upperCase(const char* str);

    inline int          checkErrors();
    int                 validateMaskInput(int input, int warningIndex, int getWarningsIndex);
    int                 validateMaskShiftInput(const char* shiftMask, int warningIndex, int getWarningsIndex);

    static double       convert_defname2num(char *versionName);

    static int          numIsInt (char* volt);
    int                 defValidNum(int values);
    int                 def60NewSyntaxError(const char *address);
    int                 def60ObsoletedError(const char *address);
    int                 def60SyntaxError(const char* rule);
    int                 def60ExclusiveStatementsError(const char *address, 
                                                      const char *paramList);
    int                 def60KeywordRequiresKeywordError(const char*    addres, 
                                                         const char*    keyword, 
                                                         const char*    requiredKeywords);
    void                addProp(defiProp* prop);
    void                cleanProps();
    void                setPropDataType(defiProp*       prop,
                                        const char*     propTypeName,
                                        defiPropType&   propType);
    void                setPropsDataTypes(const char*   propTypeName,
                                          defiPropType& propType);
    void                addNetProps();

    inline static const char   *defkywd(int num);

    FILE*  defrLog; 
    char   defPropDefType; // save the current type of the property
    char*  defMsg; 
    char*  deftoken; 
    char*  uc_token;
    char*  last; 
    char*  magic; 
    char*  next; 
    char*  pv_deftoken; 
    char*  rowName; // to hold the rowName for message
    char*  shieldName; // to hold the shieldNetName
    char*  shiftBuf; 
    char*  warningMsg; 
    double save_x; 
    double save_y; 
    double lVal;
    double rVal;
    int  aOxide; // keep track for oxide
    int  assertionWarnings; 
    int  bit_is_keyword; 
    int  bitsNum; // Scanchain Bits value
    int  blockageWarnings; 
    int  by_is_keyword; 
    int  caseSensitiveWarnings; 
    int  componentWarnings; 
    int  constraintWarnings; 
    int  cover_is_keyword; 
    int  defIgnoreVersion; // ignore checking version number
    int  defInvalidChar; 
    int  defMsgCnt; 
    int  defMsgPrinted; // number of msgs output so far
    int  defPrintTokens; 
    int  defRetVal; 
    int  def_warnings; 
    int  defaultCapWarnings; 
    int  do_is_keyword; 
    int  dumb_mode; 
    int  errors; 
    int  fillWarnings; 
    int  first_buffer; 
    int  fixed_is_keyword; 
    int  gcellGridWarnings; 
    int  hasBlkLayerComp; // only 1 BLOCKAGE/LAYER/COMP
    int  hasBlkLayerSpac; // only 1 BLOCKAGE/LAYER/SPACING
    int  hasBlkLayerTypeComp; // SLOTS or FILLS
    int  hasBlkPlaceComp; // only 1 BLOCKAGE/PLACEMENT/COMP
    int  hasBlkPlaceTypeComp; // SOFT or PARTIAL
    int  hasDef60BlkPlaceTypeComp; // DEF 6.0 ONLYBLOCKS, SOFT, or PARTIAL
    int  hasBusBit; // keep track BUSBITCHARS is in the file
    int  hasDes; // keep track DESIGN is in the file
    int  hasDivChar; // keep track DIVIDERCHAR is in the file
    int  hasDoStep; 
    int  hasNameCase; // keep track NAMESCASESENSITIVE is in the file
    int  hasOpenedDefLogFile; 
    int  hasPort; // keep track is port defined in a Pin
    int  hadPortOnce; // to restrict implicit ports if the Pin already has any port
    int  hasVer; // keep track VERSION is in the file
    int  hasFatalError; // don't report errors after the file end.
    int  iOTimingWarnings; 
    int  input_level; 
    int  mask_is_keyword; 
    int  mustjoin_is_keyword; 
    int  names_case_sensitive; // always true in 5.6
    int  needNPCbk; // if cbk for net path is needed
    int  needSNPCbk; // if cbk for snet path is needed
    int  netOsnet; // net = 1 & snet = 2
    int  netWarnings; 
    int  new_is_keyword; 
    int  nl_token; 
    int  no_num; 
    int  nonDefaultWarnings; 
    int  nondef_is_keyword; 
    int  ntokens; 
    int  orient_is_keyword; 
    int  pinExtWarnings; 
    int  pinWarnings; 
    int  real_num; 
    int  rect_is_keyword; 
    int  regTypeDef; // keep track that region type is defined 
    int  regionWarnings; 
    int  ringPlace; 
    int  routed_is_keyword; 
    int  rowWarnings; 
    int  sNetWarnings; 
    int  scanchainWarnings; 
    int  shiftBufLength; 
    int  specialWire_mask; 
    int  step_is_keyword; 
    int  stylesWarnings; 
    int  trackWarnings; 
    int  unitsWarnings; 
    int  versionWarnings; 
    int  viaRule; // keep track the viarule has called first
    int  viaWarnings; 
    int  virtual_is_keyword; 
    int  deftokenLength;
    long long nlines;
    int  width_is_keyword;

    std::vector<char>  History_text; 

    char*  routeStatus;
    char*  shapeType;
    double VersionNum;
    double xStep;
    double yStep;
        
    //defrParser vars.
    defiShield *Shield;
    defiWire *Wire;
    defiPath *PathObj;
    defiProp Prop;
    defiSite Site;
    defiComponent *Component;
    defiComponentMaskShiftLayer ComponentMaskShiftLayer;
    defiNet *Net;
    defiPinCap PinCap;
    defiSite CannotOccupy;
    defiSite Canplace;
    defiBox DieArea;
    defiPin Pin;
    defiRow Row;
    defiTrack Track;
    defiGcellGrid GcellGrid;
    defiVia Via;
    defiRegion Region;
    defiGroup Group;
    defiAssertion Assertion;
    defiScanchain Scanchain;
    defiIOTiming IOTiming;
    defiFPC FPC;
    defiTimingDisable TimingDisable;
    defiPartition Partition;
    defiPinProp PinProp;
    defiBlockage Blockage;
    defiSlot Slot;
    defiFill Fill;
    defiNonDefault NonDefault;
    defiStyles Styles;
    defiGeometries Geometries;
    defrInts       Orients;
    int doneDesign;      // keep track if the Design is done parsing

    defiSubnet* Subnet;
    int msgLimit[DEF_MSGS];
    char buffer[IN_BUF_SIZE];
    char* ring[RING_SIZE];
    int ringSizes[RING_SIZE];

    YYSTYPE yylval;
    const defrCallbacks *callbacks;
    const defrSettings  *settings;
    defrSession         *session;
    char                lineBuffer[MSG_SIZE];
    defrProps*          props;

    FILE* File;
};

class defrContext {
public:
    defrContext(int ownConf = 0);

    defrSettings          *settings;
    defrCallbacks         *callbacks;
    defrSession           *session;
    defrData              *data;
    int                   ownConfig;
    const char            *init_call_func;
};

int 
defrData::checkErrors()
{
    if (errors > settings->totalNumErrorsParsed) {
        defError(6011, "Too many syntax errors have been reported."); 
        errors = 0; 
        return 1; 
    }

    return 0;
}

END_LEFDEF_PARSER_NAMESPACE

#endif

