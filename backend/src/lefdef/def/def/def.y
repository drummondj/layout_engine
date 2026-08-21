//******************************************************************************
//******************************************************************************
// Copyright 2013-2021, Cadence Design Systems
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
//******************************************************************************
// 
//  $Author: icftcm $
//  $Revision: #36 $
//  $Date: 2025/03/27 $
//  $State:  $
//****************************************************************************

//  Error message number:
//  5000 - def reader, defrReader.cpp
//  5500 - lex.cpph, yyerror
//  6000 - def parser, error, lex.cpph, def.y (CALLBACK & CHKERR)
//  6010 - defiBlockage.cpp
//  6020 - defiComponent.cpp
//  6030 - defiFPC.cpp
//  6040 - defiFill.cpp
//  6050 - defiGroup.cpp
//  6060 - defiIOTiming.cpp
//  6070 - defiMisc.cpp
//  6080 - defiNet.cpp
//  6090 - defiNonDefault.cpp
//  6100 - defiPartition.cpp
//  6110 - defiPinCap.cpp
//  6120 - defiPinProp.cpp
//  6130 - defiRegion.cpp
//  6140 - defiRowTrack.cpp
//  6150 - defiScanchain.cpp
//  6160 - defiSlot.cpp
//  6170 - defiTimingDisable.cpp
//  6180 - defiVia.cpp
//  6500 - def parser, error, def.y
%define api.pure
%lex-param {defrData *defData}
%parse-param {defrData *defData}


%{
#include <stdlib.h>
#include <string.h>
#include "defrReader.hpp"
#include "defiUser.hpp"
#include "defrCallBacks.hpp"
#include "lex.h"

#define DEF_MAX_INT 2147483647
#define YYDEBUG 1     // this is temp fix for pcr 755132 
// TX_DIR:TRANSLATION ON


#include "defrData.hpp"
#include "defrSettings.hpp"
#include "defrCallBacks.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

// Macros to describe how we handle a callback.
// If the function was set then call it.
// If the function returns non zero then there was an error
// so call the error routine and exit.

#define PROCESS_CALLBACK_RETVAL() \
     if (defData->defRetVal == PARSE_OK) { \
     } else if (defData->defRetVal == STOP_PARSE) { \
         return defData->defRetVal; \
     } else { \
         defData->defError(6010, "An error has been reported in callback."); \
         return defData->defRetVal; \
     }

#define CALLBACK(func, typ, data) \
     if (func && !defData->errors) { \
         defData->defRetVal = (*func)(typ, data, defData->session->UserData); \
         PROCESS_CALLBACK_RETVAL(); \
     }

#define CALLBACK2(func, typ, data1, data2) \
     if (func && !defData->errors) { \
         defData->defRetVal = (*func)(typ, data1, data2, defData->session->UserData); \
         PROCESS_CALLBACK_RETVAL(); \
     }

#define CALLBACK3(func, typ, data1, data2, data3) \
     if (func && !defData->errors) { \
         defData->defRetVal = (*func)(typ, data1, data2, data3, defData->session->UserData); \
         PROCESS_CALLBACK_RETVAL(); \
     }

#define CHKERR() \
    if (defData->checkErrors()) { \
      return 1; \
    }

#define CHKPROPTYPE(propType, propName, name) \
    if (propType == 'N') { \
       defData->warningMsg = (char*)malloc(strlen(propName)+strlen(name)+40); \
       sprintf(defData->warningMsg, "The PropName %s is not defined for %s.", \
               propName, name); \
       defData->defWarning(7010, defData->warningMsg); \
       free(defData->warningMsg); \
    }

int yylex(YYSTYPE *pYylval, defrData *defData)
{
    return defData->defyylex(pYylval);
}


void yyerror(defrData *defData, const char *s)
{
    return defData->defyyerror(s);
}




#define FIXED 1
#define COVER 2
#define PLACED 3
#define UNPLACED 4
%}










%token <string>  QSTRING
%token <string>  T_STRING SITE_PATTERN
%token <dval>    NUMBER
%token <keyword> K_HISTORY K_NAME K_NAMESCASESENSITIVE
%token <keyword> K_DESIGN K_VIAS K_TECH K_UNITS K_ARRAY K_FLOORPLAN
%token <keyword> K_SITE K_CANPLACE K_CANNOTOCCUPY K_DIEAREA
%token <keyword> K_PINS K_PINSHAPE
%token <keyword> K_DEFAULTCAP K_MINPINS K_WIRECAP
%token <keyword> K_TRACKS K_GCELLGRID
%token <keyword> K_DO K_BY K_STEP K_LAYER K_ROW K_RECT
%token <keyword> K_COMPS K_COMP_GEN K_SOURCE K_WEIGHT K_EEQMASTER
%token <keyword> K_FIXED K_COVER K_UNPLACED K_PLACED K_FOREIGN K_REGION 
%token <keyword> K_REGIONS
%token <keyword> K_NETS K_START_NET K_MUSTJOIN K_ORIGINAL K_USE K_STYLE
%token <keyword> K_PATTERN K_PATTERNNAME K_ESTCAP K_ROUTED K_NEW 
%token <keyword> K_SNETS K_SHAPE K_WIDTH K_VOLTAGE K_SPACING K_NONDEFAULTRULE
%token <keyword> K_NONDEFAULTRULES K_NOFLOPS
%token <keyword> K_N K_S K_E K_W K_FN K_FE K_FS K_FW
%token <keyword> K_GROUPS K_GROUP K_SOFT K_MAXX K_MAXY K_MAXHALFPERIMETER
%token <keyword> K_CONSTRAINTS K_NET K_PATH K_SUM K_DIFF 
%token <keyword> K_SCANCHAINS K_START K_FLOATING K_ORDERED K_STOP K_IN K_OUT
%token <keyword> K_RISEMIN K_RISEMAX K_FALLMIN K_FALLMAX K_WIREDLOGIC
%token <keyword> K_MAXDIST
%token <keyword> K_ASSERTIONS
%token <keyword> K_DISTANCE K_MICRONS K_NDR
%token <keyword> K_END K_POWERDOMAIN K_HINSTS 
%token <keyword> K_IOTIMINGS K_RISE K_FALL K_VARIABLE K_SLEWRATE K_CAPACITANCE
%token <keyword> K_DRIVECELL K_FROMPIN K_TOPIN K_PARALLEL
%token <keyword> K_TIMINGDISABLES K_THRUPIN K_MACRO
%token <keyword> K_PARTITIONS K_TURNOFF K_COMPONENTS
%token <keyword> K_FROMCLOCKPIN K_FROMCOMPPIN K_FROMIOPIN
%token <keyword> K_TOCLOCKPIN K_TOCOMPPIN K_TOIOPIN
%token <keyword> K_SETUPRISE K_SETUPFALL K_HOLDRISE K_HOLDFALL
%token <keyword> K_VPIN K_SUBNET K_XTALK K_PIN K_SYNTHESIZED
%token <keyword> K_IF K_THEN K_ELSE K_FALSE K_TRUE 
%token <keyword> K_EQ K_NE K_LE K_LT K_GE K_GT K_OR K_AND K_NOT
%token <keyword> K_SPECIAL K_DIRECTION K_RANGE K_WIRE
%token <keyword> K_FPC K_HORIZONTAL K_VERTICAL K_ALIGN K_MIN K_MAX K_EQUAL
%token <keyword> K_BOTTOMLEFT K_TOPRIGHT K_ROWS K_TAPER K_TAPERRULE
%token <keyword> K_VERSION K_DIVIDERCHAR K_BUSBITCHARS
%token <keyword> K_PROPERTYDEFINITIONS K_STRING K_REAL K_INTEGER K_PROPERTY
%token <keyword> K_BEGINEXT K_ENDEXT K_NAMEMAPSTRING K_ON K_OFF K_X K_Y
%token <keyword> K_COMPONENT K_MASK K_MASKSHIFT K_COMPSMASKSHIFT K_SAMEMASK
%token <keyword> K_PINPROPERTIES K_TEST K_ONLYBLOCKS
%token <keyword> K_COMMONSCANPINS K_SNET K_COMPONENTPIN K_REENTRANTPATHS
%token <keyword> K_SHIELD K_SHIELDNET K_NOSHIELD K_VIRTUAL
%token <keyword> K_ANTENNAPINPARTIALMETALAREA K_ANTENNAPINPARTIALMETALSIDEAREA
%token <keyword> K_ANTENNAPINGATEAREA K_ANTENNAPINDIFFAREA
%token <keyword> K_ANTENNAPINMAXAREACAR K_ANTENNAPINMAXSIDEAREACAR
%token <keyword> K_ANTENNAPINPARTIALCUTAREA K_ANTENNAPINMAXCUTCAR
%token <keyword> K_SIGNAL K_POWER K_GROUND K_CLOCK K_TIEOFF K_ANALOG K_SCAN
%token <keyword> K_RESET K_RING K_STRIPE K_FOLLOWPIN K_IOWIRE K_COREWIRE
%token <keyword> K_BLOCKWIRE K_FILLWIRE K_BLOCKAGEWIRE K_PADRING K_BLOCKRING
%token <keyword> K_BLOCKAGES K_PLACEMENT K_SLOTS K_FILLS K_PUSHDOWN
%token <keyword> K_NETLIST K_DIST K_USER K_TIMING K_BALANCED K_STEINER K_TRUNK
%token <keyword> K_FIXEDBUMP K_FENCE K_FREQUENCY K_GUIDE K_MAXBITS
%token <keyword> K_PARTITION K_TYPE K_ANTENNAMODEL K_DRCFILL
%token <keyword> K_OXIDE1 K_OXIDE2 K_OXIDE3 K_OXIDE4
%token <keyword> K_OXIDE5 K_OXIDE6 K_OXIDE7 K_OXIDE8
%token <keyword> K_OXIDE9 K_OXIDE10 K_OXIDE11 K_OXIDE12
%token <keyword> K_OXIDE13 K_OXIDE14 K_OXIDE15 K_OXIDE16
%token <keyword> K_OXIDE17 K_OXIDE18 K_OXIDE19 K_OXIDE20
%token <keyword> K_OXIDE21 K_OXIDE22 K_OXIDE23 K_OXIDE24
%token <keyword> K_OXIDE25 K_OXIDE26 K_OXIDE27 K_OXIDE28
%token <keyword> K_OXIDE29 K_OXIDE30 K_OXIDE31 K_OXIDE32
%token <keyword> K_CUTSIZE K_CUTSPACING K_DESIGNRULEWIDTH K_DIAGWIDTH
%token <keyword> K_ENCLOSURE K_HALO K_GROUNDSENSITIVITY K_PHYSICAL
%token <keyword> K_HARDSPACING K_LAYERS K_MINCUTS K_NETEXPR K_PINPROPERTY
%token <keyword> K_OFFSET K_ORIGIN K_ROWCOL K_STYLES K_SOFTFIXED
%token <keyword> K_POLYGON K_PORT K_SUPPLYSENSITIVITY K_VIA K_VIARULE K_WIREEXT
%token <keyword> K_EXCEPTPGNET K_ONLYPGNET K_FILLWIREOPC K_OPC K_PARTIAL K_ROUTEHALO
%token <keyword> K_BLOCKAGE K_ROUTE K_SCANCHAIN K_SPECIALROUTE K_TRACK
%type <pt>      pt opt_paren
%type <integer> comp_net_list subnet_opt_syn
%type <integer> orient pin_via_mask_opt via_orient
%type <integer> placement_status fill_via_orient
%type <string>  subnet_type track_start use_type shape_type source_type
%type <string>  snets_pattern_type nets_pattern_type netsource_type prop_string_value
%type <tk>      path paths new_path
%type <string>  risefall opt_pin opt_pattern pin_layer_opt
%type <string>  vpin_status opt_plus track_type region_type
%type <string>  h_or_v turnoff_setup turnoff_hold
%type <integer> conn_opt partition_maxbits same_mask mask orient_pt 
%type <prop> prop_name_value prop_name_value_pair 

%%

def_file: version_stmt case_sens_stmt rules end_design
            ;

version_stmt:  // empty 
    | K_VERSION { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ';'
      {
        defData->VersionNum = defrData::convert_defname2num($3);
        if (defData->VersionNum == 0 || defData->VersionNum > CURRENT_VERSION + 0.0001) {
          char temp[300];
          sprintf(temp,
                  "The execution has been stopped because the DEF parser %.2f does not support DEF file with version '%s'. Update your DEF file to version %.2f or earlier.",
                  CURRENT_VERSION, 
                  $3, 
                  CURRENT_VERSION);
          defData->defError(6503, temp);
          return 1;
        }

        if (!defData->settings->AllowVer60Plus && defData->VersionNum > 5.8001) {
          char temp[300];
          sprintf(temp,
                  "The execution has been stopped because DEF parser can process %.2f version DEF files, but the translator software does not support processing of DEF files with version greater than 5.8. Update the translator software.",
                  CURRENT_VERSION);
          defData->defError(6565, temp);
          return 1;
        }

        if (defData->callbacks->VersionStrCbk) {
          CALLBACK(defData->callbacks->VersionStrCbk, defrVersionStrCbkType, $3);
        } else if (defData->callbacks->VersionCbk) {
            CALLBACK(defData->callbacks->VersionCbk, defrVersionCbkType, defData->VersionNum);
        }
        if (defData->VersionNum > 5.3 && defData->VersionNum < 5.4)
          defData->defIgnoreVersion = 1;
        if (defData->VersionNum < 5.6)     // default to false before 5.6
          defData->names_case_sensitive = defData->session->reader_case_sensitive;
        else
          defData->names_case_sensitive = 1;
        defData->hasVer = 1;
        defData->doneDesign = 0;
    }

case_sens_stmt: // empty 
    | K_NAMESCASESENSITIVE K_ON ';'
      {
        if (defData->VersionNum < 5.6) {
          defData->names_case_sensitive = 1;
          if (defData->callbacks->CaseSensitiveCbk)
            CALLBACK(defData->callbacks->CaseSensitiveCbk, defrCaseSensitiveCbkType,
                     defData->names_case_sensitive); 
          defData->hasNameCase = 1;
        } else
          if (defData->callbacks->CaseSensitiveCbk) // write error only if cbk is set 
             if (defData->caseSensitiveWarnings++ < defData->settings->CaseSensitiveWarnings)
               defData->defWarning(7011, "The NAMESCASESENSITIVE statement is obsolete in version 5.6 and later.\nThe DEF parser will ignore this statement.");
      }
    | K_NAMESCASESENSITIVE K_OFF ';'
      {
        if (defData->VersionNum < 5.6) {
          defData->names_case_sensitive = 0;
          if (defData->callbacks->CaseSensitiveCbk)
            CALLBACK(defData->callbacks->CaseSensitiveCbk, defrCaseSensitiveCbkType,
                     defData->names_case_sensitive);
          defData->hasNameCase = 1;
        } else {
          if (defData->callbacks->CaseSensitiveCbk) { // write error only if cbk is set 
            if (defData->caseSensitiveWarnings++ < defData->settings->CaseSensitiveWarnings) {
              defData->defError(6504, "Def parser version 5.7 and later does not support NAMESCASESENSITIVE OFF.\nEither remove this optional construct or set it to ON.");
              CHKERR();
            }
          }
        }
      }

rules: // empty 
        | rules rule
        | error 
            ;

rule: design_section | assertions_section | blockage_section | comps_section
      | constraint_section | extension_section | fill_section | comps_maskShift_section
      | floorplan_contraints_section | groups_section | iotiming_section
      | nets_section |  nondefaultrule_section | partitions_section
      | pin_props_section | regions_section | scanchains_section
      | slot_section | snets_section | styles_section | timingdisables_section
      | via_section
            ;

design_section: array_name | bus_bit_chars | canplace | cannotoccupy |
              design_name | die_area | divider_char | 
              floorplan_name | gcellgrid | history |
              pin_cap_rule | pin_rule | prop_def_section |
              row_rule | tech_name | tracks_rule | units
            ;



design_name: K_DESIGN {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ';' 
      {
            if (defData->callbacks->DesignCbk)
              CALLBACK(defData->callbacks->DesignCbk, defrDesignStartCbkType, $3);
            defData->hasDes = 1;
          }

end_design: K_END K_DESIGN
          {
            defData->doneDesign = 1;
            if (defData->callbacks->DesignEndCbk)
              CALLBACK(defData->callbacks->DesignEndCbk, defrDesignEndCbkType, 0);
            // 11/16/2001 - pcr 408334
            // Return 1 if there is any defData->errors during parsing
            if (defData->errors)
                return 1;

            if (!defData->hasVer) {
              char temp[300];
              sprintf(temp, "No VERSION statement found, using the default value %2g.", defData->VersionNum);
              defData->defWarning(7012, temp);            
            }
            if (!defData->hasNameCase && defData->VersionNum < 5.6)
              defData->defWarning(7013, "The DEF file is invalid if NAMESCASESENSITIVE is undefined.\nNAMESCASESENSITIVE is a mandatory statement in the DEF file with version 5.6 and earlier.\nTo define the NAMESCASESENSITIVE statement, refer to the LEF/DEF 5.5 and earlier Language Reference manual.");
            if (!defData->hasBusBit && defData->VersionNum < 5.6)
              defData->defWarning(7014, "The DEF file is invalid if BUSBITCHARS is undefined.\nBUSBITCHARS is a mandatory statement in the DEF file with version 5.6 and earlier.\nTo define the BUSBITCHARS statement, refer to the LEF/DEF 5.5 and earlier Language Reference manual.");
            if (!defData->hasDivChar && defData->VersionNum < 5.6)
              defData->defWarning(7015, "The DEF file is invalid if DIVIDERCHAR is undefined.\nDIVIDERCHAR is a mandatory statement in the DEF file with version 5.6 and earlier.\nTo define the DIVIDERCHAR statement, refer to the LEF/DEF 5.5 and earlier Language Reference manual.");
            if (!defData->hasDes)
              defData->defWarning(7016, "DESIGN is a mandatory statement in the DEF file. Ensure that it exists in the file.");
          }

tech_name: K_TECH { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ';'
          { 
            if (defData->callbacks->TechnologyCbk)
              CALLBACK(defData->callbacks->TechnologyCbk, defrTechNameCbkType, $3);
          }

array_name: K_ARRAY {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING ';'
          { 
            if (defData->callbacks->ArrayNameCbk)
              CALLBACK(defData->callbacks->ArrayNameCbk, defrArrayNameCbkType, $3);
          }

floorplan_name: K_FLOORPLAN { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ';'
          { 
            if (defData->callbacks->FloorPlanNameCbk)
              CALLBACK(defData->callbacks->FloorPlanNameCbk, defrFloorPlanNameCbkType, $3);
          }

history: K_HISTORY
          { 
            if (defData->callbacks->HistoryCbk)
              CALLBACK(defData->callbacks->HistoryCbk, defrHistoryCbkType, &defData->History_text[0]);
          }

prop_def_section: K_PROPERTYDEFINITIONS
          {
            if (defData->callbacks->PropDefStartCbk)
              CALLBACK(defData->callbacks->PropDefStartCbk, defrPropDefStartCbkType, 0);
          }
    property_defs K_END K_PROPERTYDEFINITIONS
          { 
            if (defData->callbacks->PropDefEndCbk)
              CALLBACK(defData->callbacks->PropDefEndCbk, defrPropDefEndCbkType, 0);
            defData->real_num = 0;     // just want to make sure it is reset 
          }

property_defs: // empty 
        | property_defs property_def
            { }

property_def: K_DESIGN {defData->dumb_mode = 1; defData->no_num = 1; defData->Prop.clear(); }
              T_STRING property_type_and_val ';' 
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("design", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->DesignProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_NET { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("net", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->NetProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_SNET { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("specialnet", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->SNetProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_REGION { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("region", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->RegionProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_GROUP { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("group", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->GroupProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_COMPONENT { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("component", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->CompProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_ROW { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("row", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->RowProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }

        | K_COMPONENTPIN
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->callbacks->PropCbk) {
                defData->Prop.setPropType("componentpin", $3);
                CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
              }
              defData->session->CompPinProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
            }
        | K_NONDEFAULTRULE
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
            {
              if (defData->VersionNum < 5.6) {
                if (defData->nonDefaultWarnings++ < defData->settings->NonDefaultWarnings) {
                  defData->defMsg = (char*)malloc(1000); 
                  sprintf (defData->defMsg,
                     "The NONDEFAULTRULE statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6505, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              } else {
                if (defData->callbacks->PropCbk) {
                  defData->Prop.setPropType("nondefaultrule", $3);
                  CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                }
                defData->session->NDefProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
              }
            }
        | K_BLOCKAGE
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
          {
                if (defData->VersionNum < 6.0 - 0.00001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [BLOCKAGE propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                  if (defData->callbacks->PropCbk) {
                    defData->Prop.setPropType("blockage", $3);
                    CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                  }
                  defData->session->BlockageProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
               }
           }
        | K_PIN
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
          {
                if (defData->VersionNum < 6.0 - 0.00001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [PIN propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                  if (defData->callbacks->PropCbk) {
                    defData->Prop.setPropType("pin", $3);
                    CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                  }
                  defData->session->PinProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
               }
           }
        | K_PINSHAPE
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
          {
                if (defData->VersionNum < 6.0 - 0.00001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [PINSHAPE propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                  if (defData->callbacks->PropCbk) {
                    defData->Prop.setPropType("pinshape", $3);
                    CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                  }
                  defData->session->PinPropShape.setPropType(defData->DEFCASE($3), defData->defPropDefType);
               }
           }        
        | K_ROUTE
          { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
          T_STRING property_type_and_val ';'
          {
                if (defData->VersionNum < 6.0 - 0.0001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [ROUTE propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                  if (defData->callbacks->PropCbk) {
                    defData->Prop.setPropType("route", $3);
                    CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                  }
                  defData->session->RouteProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
               }
           }
         | K_SCANCHAIN
            { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
            T_STRING property_type_and_val ';'
            {
                if (defData->VersionNum < 6.0 - 0.0001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [SCANCHAIN ...]")) {
                        CHKERR();
                    }
                } else {
                    if (defData->callbacks->PropCbk) {
                        defData->Prop.setPropType("scanchain", $3);
                        CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop);
                    }
                    defData->session->ScanChainProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
                }
            }
         | K_SPECIALROUTE
            { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
            T_STRING property_type_and_val ';'
            {
                if (defData->VersionNum < 6.0 - 0.0001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [SPECIALROUTE propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                    if (defData->callbacks->PropCbk) {
                        defData->Prop.setPropType("specialroute", $3);
                        CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop)
                    }
                    defData->session->SpecialRouteProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
                }
            }
         | K_VIA
            { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
            T_STRING property_type_and_val ';'
            {
                if (defData->VersionNum < 6.0 - 0.0001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [VIA propName propType ... ]")) {
                        CHKERR();
                    }
                } else {
                    if (defData->callbacks->PropCbk) {
                        defData->Prop.setPropType("via", $3);
                        CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop)
                    }
                    defData->session->ViaProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
                }
            }
         | K_TRACK
            { defData->dumb_mode = 1 ; defData->no_num = 1; defData->Prop.clear(); }
            T_STRING property_type_and_val ';'
            {
                if (defData->VersionNum < 6.0 - 0.0001) {
                    if (defData->def60NewSyntaxError("PROPERTYDEFINITIONS ... [TRACK propName propType ...]")) {
                        CHKERR();
                    }
                } else {
                    if (defData->callbacks->PropCbk) {
                        defData->Prop.setPropType("track", $3);
                        CALLBACK(defData->callbacks->PropCbk, defrPropCbkType, &defData->Prop)
                    }
                    defData->session->TrackProp.setPropType(defData->DEFCASE($3), defData->defPropDefType);
                }
            }
        | error ';' { yyerrok; yyclearin;}

property_type_and_val: K_INTEGER { defData->real_num = 0; } opt_range opt_num_val
            {
              if (defData->callbacks->PropCbk) defData->Prop.setPropInteger();
              defData->defPropDefType = 'I';
            }
        | K_REAL { defData->real_num = 1; } opt_range opt_num_val
            {
              if (defData->callbacks->PropCbk) defData->Prop.setPropReal();
              defData->defPropDefType = 'R';
              defData->real_num = 0;
            }
        | K_STRING
            {
              if (defData->callbacks->PropCbk) defData->Prop.setPropString();
              defData->defPropDefType = 'S';
            }
        | K_STRING QSTRING
            {
              if (defData->callbacks->PropCbk) defData->Prop.setPropQString($2);
              defData->defPropDefType = 'Q';
            }
        | K_NAMEMAPSTRING T_STRING
            {
              if (defData->callbacks->PropCbk) defData->Prop.setPropNameMapString($2);
              defData->defPropDefType = 'S';
            }

opt_num_val: // empty 
        | NUMBER
            { if (defData->callbacks->PropCbk) defData->Prop.setNumber($1); }

units: K_UNITS K_DISTANCE K_MICRONS NUMBER ';'
          {
            if (defData->callbacks->UnitsCbk) {
              if (defData->defValidNum((int)$4))
                CALLBACK(defData->callbacks->UnitsCbk,  defrUnitsCbkType, $4);
            }
          }

divider_char: K_DIVIDERCHAR QSTRING ';'
          {
            if (defData->callbacks->DividerCbk)
              CALLBACK(defData->callbacks->DividerCbk, defrDividerCbkType, $2);
            defData->hasDivChar = 1;
          }

bus_bit_chars: K_BUSBITCHARS QSTRING ';'
          { 
            if (defData->callbacks->BusBitCbk)
              CALLBACK(defData->callbacks->BusBitCbk, defrBusBitCbkType, $2);
            defData->hasBusBit = 1;
          }

canplace: K_CANPLACE {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING NUMBER NUMBER
            orient K_DO NUMBER  K_BY NUMBER  K_STEP NUMBER NUMBER ';' 
            {
              if (defData->callbacks->CanplaceCbk) {
                defData->Canplace.setName($3);
                defData->Canplace.setLocation($4,$5);
                defData->Canplace.setOrient($6);
                defData->Canplace.setDo($8,$10,$12,$13);
                CALLBACK(defData->callbacks->CanplaceCbk, defrCanplaceCbkType,
                &(defData->Canplace));
              }
            }
cannotoccupy: K_CANNOTOCCUPY {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING NUMBER NUMBER
            orient K_DO NUMBER  K_BY NUMBER  K_STEP NUMBER NUMBER ';' 
            {
              if (defData->callbacks->CannotOccupyCbk) {
                defData->CannotOccupy.setName($3);
                defData->CannotOccupy.setLocation($4,$5);
                defData->CannotOccupy.setOrient($6);
                defData->CannotOccupy.setDo($8,$10,$12,$13);
                CALLBACK(defData->callbacks->CannotOccupyCbk, defrCannotOccupyCbkType,
                        &(defData->CannotOccupy));
              }
            }

orient: K_N    {$$ = 0;}
        | K_W  {$$ = 1;}
        | K_S  {$$ = 2;}
        | K_E  {$$ = 3;}
        | K_FN {$$ = 4;}
        | K_FW {$$ = 5;}
        | K_FS {$$ = 6;}
        | K_FE {$$ = 7;}

die_area: K_DIEAREA
          {
            defData->Geometries.Reset();
          }
          firstPt nextPt otherPts ';'
          {
            if (defData->callbacks->DieAreaCbk) {
               defData->DieArea.addPoint(&defData->Geometries);
               CALLBACK(defData->callbacks->DieAreaCbk, defrDieAreaCbkType, &(defData->DieArea));
            }
          }

// 8/31/2001 - This is obsolete in 5.4 
pin_cap_rule: start_def_cap pin_caps end_def_cap
            { }

start_def_cap: K_DEFAULTCAP NUMBER 
        {
          if (defData->VersionNum < 5.4) {
             if (defData->callbacks->DefaultCapCbk)
                CALLBACK(defData->callbacks->DefaultCapCbk, defrDefaultCapCbkType, ROUND($2));
          } else {
             if (defData->callbacks->DefaultCapCbk) // write error only if cbk is set 
                if (defData->defaultCapWarnings++ < defData->settings->DefaultCapWarnings)
                   defData->defWarning(7017, "The DEFAULTCAP statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
          }
        }

pin_caps: // empty 
        | pin_caps pin_cap
            ;

pin_cap: K_MINPINS NUMBER K_WIRECAP NUMBER ';'
          {
            if (defData->VersionNum < 5.4) {
              if (defData->callbacks->PinCapCbk) {
                defData->PinCap.setPin(ROUND($2));
                defData->PinCap.setCap($4);
                CALLBACK(defData->callbacks->PinCapCbk, defrPinCapCbkType, &(defData->PinCap));
              }
            }
          }

end_def_cap: K_END K_DEFAULTCAP 
            { }

pin_rule: start_pins pins end_pins
            { }

start_pins: K_PINS NUMBER ';'
          { 
            if (defData->callbacks->StartPinsCbk)
              CALLBACK(defData->callbacks->StartPinsCbk, defrStartPinsCbkType, ROUND($2));
          }

pins: // empty 
        | pins pin
            ;

pin: '-' {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING '+' K_NET
         {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
          {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              defData->Pin.Setup($3, $7);
            }
            defData->hasPort = 0;
            defData->hadPortOnce = 0;
          }
        pin_options ';'
          { 
            if (defData->callbacks->PinCbk)
              CALLBACK(defData->callbacks->PinCbk, defrPinCbkType, &defData->Pin);
          }

pin_options: // empty 
        | pin_options pin_option

pin_option: '+' K_SPECIAL
          {
            if (defData->callbacks->PinCbk)
              defData->Pin.setSpecial();
          }

        | extension_stmt
          { 
            if (defData->callbacks->PinExtCbk)
              CALLBACK(defData->callbacks->PinExtCbk, defrPinExtCbkType, &defData->History_text[0]);
          }

        | '+' K_DIRECTION T_STRING
          {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.setDirection($3);
          }

        | '+' K_PROPERTY 
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + PROPERTY {propName propVal}...")) {
                    CHKERR();
                }
            } 
            defData->dumb_mode = DEF_MAX_INT; 
          }
          pin_prop_name_values
          {
            defData->dumb_mode = 0; 
          }

        | '+' K_NETEXPR QSTRING
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The NETEXPR statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6506, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("PINS ... + NETEXPR \"netExprPropName defaultNetName\"")) {
                    CHKERR();
                }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                defData->Pin.setNetExpr($3);
              }
            }
          }

        | '+' K_SUPPLYSENSITIVITY { defData->dumb_mode = 1; } T_STRING
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The SUPPLYSENSITIVITY statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6507, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
                defData->Pin.setSupplySens($4);
            }
          }

        | '+' K_GROUNDSENSITIVITY { defData->dumb_mode = 1; } T_STRING
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The GROUNDSENSITIVITY statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6508, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
                defData->Pin.setGroundSens($4);
            }
          }

        | '+' K_USE use_type
          {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) defData->Pin.setUse($3);
          }
        | '+' K_PORT          // 5.7 
          {
            if (defData->VersionNum < 5.7) {
               if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                 if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                     (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                   defData->defMsg = (char*)malloc(10000);
                   sprintf (defData->defMsg,
                     "The PORT in PINS is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                   defData->defError(6555, defData->defMsg);
                   free(defData->defMsg);
                   CHKERR();
                 }
               }
            } else {
               if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                   defData->Pin.addPort();
               }

               defData->hasPort = 1;
               defData->hadPortOnce = 1;
            }
          }

        | '+' K_LAYER { defData->dumb_mode = 1; } T_STRING
          {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              if (defData->hasPort) {
                 defData->Pin.addPortLayer($4);
              } else if (defData->hadPortOnce) {
                 if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                   (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                   defData->defError(7418, "syntax error");
                   CHKERR();
                 }
              } else {
                 defData->Pin.addLayer($4);
              }
            }
          }
          pin_layer_mask_opt pin_layer_spacing_opt pin_layer_props_opt pt pt
          {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              if (defData->hasPort)
                 defData->Pin.addPortLayerPts($9.x, $9.y, $10.x, $10.y);
              else if (!defData->hadPortOnce)
                 defData->Pin.addLayerPts($9.x, $9.y, $10.x, $10.y);
            }
          }

        | '+' K_POLYGON { defData->dumb_mode = 1; } T_STRING
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The POLYGON statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6509, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort) {
                   defData->Pin.addPortPolygon($4);
                } else if (defData->hadPortOnce) {
                   if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                     (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                     defData->defError(7418, "syntax error");
                     CHKERR();
                   }
                } else {
                   defData->Pin.addPolygon($4);
                }
              }
            }
            
            defData->Geometries.Reset();            
          }
          pin_poly_mask_opt pin_poly_spacing_opt pin_poly_props_opt firstPt nextPt nextPt otherPts
          {
            if (defData->VersionNum >= 5.6) {  // only add if 5.6 or beyond
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortPolygonPts(&defData->Geometries);
                else if (!defData->hadPortOnce)
                   defData->Pin.addPolygonPts(&defData->Geometries);
              }
            }
          }
        | '+' K_VIA { defData->dumb_mode = 1; } T_STRING pin_via_mask_opt pin_via_props_opt via_orient '(' NUMBER NUMBER ')'   // 5.7 6.0
          {
            if (defData->VersionNum < 5.7) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The PIN VIA statement is available in version 5.7 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6556, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort) {
                   defData->Pin.addPortVia($4, 
                                           (int)$9,
                                           (int)$10, 
                                           $5, 
                                           $7);
                } else if (defData->hadPortOnce) {
                   if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                     (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                     defData->defError(7418, "syntax error");
                     CHKERR();
                   }
                } else {
                   defData->Pin.addVia($4, 
                                       (int)$9,
                                       (int)$10, 
                                       $5, 
                                       $7);
                }
              }
            }
          }
        | placement_status
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + COVER|FIXED|PLACED")) {
                    CHKERR();
                }
            } 

            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              if (defData->hasPort) {
                 defData->Pin.setPortPlacement($1, 0, 0, 0);
                 defData->hasPort = 0;
                 defData->hadPortOnce = 1;
              } else if (defData->hadPortOnce) {
                 if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                   (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                   defData->defError(7418, "syntax error");
                   CHKERR();
                 }
              } else {
                 defData->Pin.setPlacement($1, 0, 0, 0);
              }
            }
          }
        
        | placement_status pt orient
          {
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("PINS ... + COVER|FIXED|PLACED pt orient")) {
                    CHKERR();
                }
            } 

            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              if (defData->hasPort) {
                 defData->Pin.setPortPlacement($1, $2.x, $2.y, $3);
                 defData->hasPort = 0;
                 defData->hadPortOnce = 1;
              } else if (defData->hadPortOnce) {
                 if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                   (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                   defData->defError(7418, "syntax error");
                   CHKERR();
                 }
              } else {
                 defData->Pin.setPlacement($1, $2.x, $2.y, $3);
              }
            }
          }

        // The following is 5.4 syntax 
        | '+' K_ANTENNAPINPARTIALMETALAREA NUMBER pin_layer_opt
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINPARTIALMETALAREA statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6510, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinPartialMetalArea((int)$3, $4); 
          }
        | '+' K_ANTENNAPINPARTIALMETALSIDEAREA NUMBER pin_layer_opt
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINPARTIALMETALSIDEAREA statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6511, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinPartialMetalSideArea((int)$3, $4); 
          }
        | '+' K_ANTENNAPINGATEAREA NUMBER pin_layer_opt
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINGATEAREA statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6512, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
                defData->Pin.addAPinGateArea((int)$3, $4); 
            }
        | '+' K_ANTENNAPINDIFFAREA NUMBER pin_layer_opt
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINDIFFAREA statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6513, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinDiffArea((int)$3, $4); 
          }
        | '+' K_ANTENNAPINMAXAREACAR NUMBER K_LAYER {defData->dumb_mode=1;} T_STRING
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINMAXAREACAR statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6514, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinMaxAreaCar((int)$3, $6); 
          }
        | '+' K_ANTENNAPINMAXSIDEAREACAR NUMBER K_LAYER {defData->dumb_mode=1;}
           T_STRING
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINMAXSIDEAREACAR statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6515, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinMaxSideAreaCar((int)$3, $6); 
          }
        | '+' K_ANTENNAPINPARTIALCUTAREA NUMBER pin_layer_opt
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINPARTIALCUTAREA statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6516, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinPartialCutArea((int)$3, $4); 
          }
        | '+' K_ANTENNAPINMAXCUTCAR NUMBER K_LAYER {defData->dumb_mode=1;} T_STRING
          {
            if (defData->VersionNum <= 5.3) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAPINMAXCUTCAR statement is available in version 5.4 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6517, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAPinMaxCutCar((int)$3, $6); 
          }
        | '+' K_ANTENNAMODEL pin_oxide
          {  // 5.5 syntax 
            if (defData->VersionNum < 5.5) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The ANTENNAMODEL statement is available in version 5.5 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6518, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            }
          }

pin_prop_name_values:
        |  pin_prop_name_values prop_name_value
        {
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                defData->setPropDataType($2, "PIN", defData->session->PinProp);
                defData->Pin.addProp($2);
                $2 = 0;
            }

            delete $2;
        }
prop_name_value : 
        {
        }
        prop_name_value_pair
        {
            $$ = $2;
        }

prop_name_value_pair: 
        T_STRING NUMBER
        {
            char*  str = defData->ringCopy("                       ");
            // For backword compatibility, also set the string value 
            sprintf(str, "%g", $2);

            $$ = new defiProp(defData);
            $$->setPropQString(str);
            $$->setNumber($2);
            $$->setPropType("", $1);
            $$->setPropInteger();
        }

      | T_STRING prop_string_value
        {
            $$ = new defiProp(defData);
            $$->setPropQString($2);
            $$->setPropString();
            $$->setPropType("", $1);
        }

net_prop_name_values:
    |  net_prop_name_values prop_name_value
    {
        defData->addProp($2);
    }

prop_string_value: T_STRING 
        {
            $$ = $1;
        }
    | QSTRING
        {
            $$ = $1;
        }
via_orient: // empty
          { 
            $$ = 0;
          }
        | orient
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + VIA viaName [MASK] orient pt")) {
                    CHKERR();
                }
            } 

            $$ = $1;
          }
pin_layer_mask_opt: // empty 
        | K_MASK NUMBER
         { 
           if (defData->validateMaskInput((int)$2, defData->pinWarnings, defData->settings->PinWarnings)) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortLayerMask((int)$2);
                else
                   defData->Pin.addLayerMask((int)$2);
              }
           }
         }
         
pin_via_mask_opt: 
        // empty 
        { $$ = 0; }
        | K_MASK NUMBER
         { 
           if (defData->validateMaskInput((int)$2, defData->pinWarnings, defData->settings->PinWarnings)) {
             $$ = $2;
           }
         }
         
pin_poly_mask_opt: // empty 
        | K_MASK NUMBER
         { 
           if (defData->validateMaskInput((int)$2, defData->pinWarnings, defData->settings->PinWarnings)) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortPolyMask((int)$2);
                else
                   defData->Pin.addPolyMask((int)$2);
              }
           }
         }
   
pin_layer_spacing_opt: 
    | pin_layer_spacing
          {
          }

pin_layer_props_opt: 
    | pin_layer_props
    {}

pin_layer_props: pin_layer_props pin_layer_prop
    | pin_layer_prop
    {}

pin_layer_prop: K_PROPERTY 
          { 
            defData->dumb_mode = 2; 
          } 
          prop_name_value
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + PORT ... + LAYER ... PROPERTY propName propValue")) {
                    CHKERR();
                }
            }

            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              defData->setPropDataType($3, "PINSHAPE", defData->session->PinPropShape);

              if (defData->hasPort) {
                defData->Pin.addPortLayerProp($3);
              } else {
                defData->Pin.addLayerProp($3);     
              }

                $3 = 0;
              }

            delete $3;
          }

pin_poly_props_opt: 
    | pin_poly_props
    {}

pin_poly_props: pin_poly_props pin_poly_prop
    | pin_poly_prop
    {}

pin_poly_prop: K_PROPERTY 
          { 
            defData->dumb_mode = 2; 
          } 
          prop_name_value
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + PORT ... + POLYGON ... PROPERTY propName propValue")) {
                    CHKERR();
                }
            }

            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
              defData->setPropDataType($3, "PINSHAPE", defData->session->PinPropShape);

              if (defData->hasPort) {
                defData->Pin.addPortPolyProp($3);
              } else {
                defData->Pin.addPolyProp($3);     
              }

                $3 = 0;
              }

            delete $3;
          }

pin_via_props_opt: 
    | pin_via_props
    {}

pin_via_props: pin_via_props pin_via_prop
        | pin_via_prop
        {}
        
pin_via_prop: K_PROPERTY 
          { 
            defData->dumb_mode = 2; 
          } 
          prop_name_value 
          {
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("PINS ... + PORT ... + VIA ... PROPERTY propName propValue")) {
                    CHKERR();
                }
            }

            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                defData->setPropDataType($3, "PINSHAPE", defData->session->PinPropShape);

              if (defData->hasPort) {
                defData->Pin.addPortViaProp($3);
                } else {
                    defData->Pin.addViaProp($3);     
                }

                $3 = 0;
              }

            delete $3;
          }

pin_layer_spacing: // empty 
        K_SPACING NUMBER
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The SPACING statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6519, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("PINS ... + LAYER SPACING minSpacing")) {
                    CHKERR();
                }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortLayerSpacing((int)$2);
                else
                   defData->Pin.addLayerSpacing((int)$2);
              }
            }
          }
        | K_DESIGNRULEWIDTH NUMBER
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "DESIGNRULEWIDTH statement is a version 5.6 and later syntax.\nYour def file is defined with version %.2f", defData->VersionNum);
                  defData->defError(6520, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else if (defData->VersionNum >= 6.0 - 0.00001) {
              if (defData->def60ObsoletedError("PINS ... + LAYER ... DESIGNRULEWIDTH effectiveWidth")) {
                CHKERR();
              }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortLayerDesignRuleWidth((int)$2);
                else
                   defData->Pin.addLayerDesignRuleWidth((int)$2);
              }
            }
          }

pin_poly_spacing_opt: // empty 
        | pin_poly_spacing
          {
          }

pin_poly_spacing:        
        K_SPACING NUMBER
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "SPACING statement is a version 5.6 and later syntax.\nYour def file is defined with version %.2f", defData->VersionNum);
                  defData->defError(6521, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("PINS ... + POLYGON SPACING minSpacing")) {
                    CHKERR();
                }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortPolySpacing((int)$2);
                else
                   defData->Pin.addPolySpacing((int)$2);
              }
            }
          }
        | K_DESIGNRULEWIDTH NUMBER
          {
            if (defData->VersionNum < 5.6) {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if ((defData->pinWarnings++ < defData->settings->PinWarnings) &&
                    (defData->pinWarnings++ < defData->settings->PinExtWarnings)) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The DESIGNRULEWIDTH statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                  defData->defError(6520, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
            } else if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("PINS ... + POLYGON ... DESIGNRULEWIDTH effectiveWidth")) {
                    CHKERR();
                }
            } else {
              if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk) {
                if (defData->hasPort)
                   defData->Pin.addPortPolyDesignRuleWidth((int)$2);
                else
                   defData->Pin.addPolyDesignRuleWidth((int)$2);
              }
            }
          }

pin_oxide: K_OXIDE1
          { defData->aOxide = 1;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE2
          { defData->aOxide = 2;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE3
          { defData->aOxide = 3;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE4
          { defData->aOxide = 4;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE5 
          { defData->aOxide = 5;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE6
          { defData->aOxide = 6;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE7
          { defData->aOxide = 7;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE8
          { defData->aOxide = 8;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE9 
          { defData->aOxide = 9;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE10
          { defData->aOxide = 10;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE11
          { defData->aOxide = 11;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE12
          { defData->aOxide = 12;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE13 
          { defData->aOxide = 13;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE14
          { defData->aOxide = 14;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE15
          { defData->aOxide = 15;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE16
          { defData->aOxide = 16;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE17
          { defData->aOxide = 17;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE18
          { defData->aOxide = 18;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE19
          { defData->aOxide = 19;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE20
          { defData->aOxide = 20;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE21
          { defData->aOxide = 21;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE22
          { defData->aOxide = 22;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE23
          { defData->aOxide = 23;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE24
          { defData->aOxide = 24;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE25
          { defData->aOxide = 25;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE26
          { defData->aOxide = 26;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE27
          { defData->aOxide = 27;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE28
          { defData->aOxide = 28;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE29 
          { defData->aOxide = 29;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE30
          { defData->aOxide = 30;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE31
          { defData->aOxide = 31;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }
        | K_OXIDE32
          { defData->aOxide = 32;
            if (defData->callbacks->PinCbk || defData->callbacks->PinExtCbk)
              defData->Pin.addAntennaModel(defData->aOxide);
          }


use_type: K_SIGNAL
          { $$ = (char*)"SIGNAL"; }
        | K_POWER
          { $$ = (char*)"POWER"; }
        | K_GROUND
          { $$ = (char*)"GROUND"; }
        | K_CLOCK
          { $$ = (char*)"CLOCK"; }
        | K_TIEOFF
          { $$ = (char*)"TIEOFF"; }
        | K_ANALOG
          { $$ = (char*)"ANALOG"; }
        | K_SCAN
          { $$ = (char*)"SCAN"; }
        | K_RESET
          { $$ = (char*)"RESET"; }

pin_layer_opt:
        // empty 
          { $$ = (char*)""; }
        | K_LAYER {defData->dumb_mode=1;} T_STRING
          { $$ = $3; }

end_pins: K_END K_PINS
        { 
          if (defData->callbacks->PinEndCbk)
            CALLBACK(defData->callbacks->PinEndCbk, defrPinEndCbkType, 0);
        }

row_rule: K_ROW {defData->dumb_mode = 2; defData->no_num = 2; } T_STRING T_STRING NUMBER NUMBER
      orient
        {
          if (defData->callbacks->RowCbk) {
            defData->rowName = $3;
            defData->Row.setup($3, $4, $5, $6, $7);
          }
        }
      row_do_option
      row_options ';'
        {
          if (defData->callbacks->RowCbk) 
            CALLBACK(defData->callbacks->RowCbk, defrRowCbkType, &defData->Row);
        }

row_do_option: // empty 
        {
          if (defData->VersionNum < 5.6) {
            if (defData->callbacks->RowCbk) {
              if (defData->rowWarnings++ < defData->settings->RowWarnings) {
                defData->defError(6523, "Invalid ROW statement defined in the DEF file. The DO statement which is required in the ROW statement is not defined.\nUpdate your DEF file with a DO statement.");
                CHKERR();
              }
            }
          }
        }
      | K_DO NUMBER K_BY NUMBER row_step_option
        {
          // 06/05/2002 - pcr 448455 
          // Check for 1 and 0 in the correct position 
          // 07/26/2002 - Commented out due to pcr 459218 
          if (defData->hasDoStep) {
            // 04/29/2004 - pcr 695535 
            // changed the testing 
            if ((($4 == 1) && (defData->yStep == 0)) ||
                (($2 == 1) && (defData->xStep == 0))) {
              // do nothing 
            } else 
              if (defData->VersionNum < 5.6) {
                if (defData->callbacks->RowCbk) {
                  if (defData->rowWarnings++ < defData->settings->RowWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf(defData->defMsg,
                            "The DO statement in the ROW statement with the name %s has invalid syntax.\nThe valid syntax is \"DO numX BY 1 STEP spaceX 0 | DO 1 BY numY STEP 0 spaceY\".\nSpecify the valid syntax and try again.", defData->rowName);
                    defData->defWarning(7018, defData->defMsg);
                    free(defData->defMsg);
                    }
                  }
              }
          }
          // pcr 459218 - Error if at least numX or numY does not equal 1 
          if (($2 != 1) && ($4 != 1)) {
            if (defData->callbacks->RowCbk) {
              if (defData->rowWarnings++ < defData->settings->RowWarnings) {
                defData->defError(6524, "Invalid syntax specified. The valid syntax is either \"DO 1 BY num or DO num BY 1\". Specify the valid syntax and try again.");
                CHKERR();
              }
            }
          }
          if (defData->callbacks->RowCbk)
            defData->Row.setDo(ROUND($2), ROUND($4), defData->xStep, defData->yStep);
        }

row_step_option: // empty 
        {
          defData->hasDoStep = 0;
        }
      | K_STEP NUMBER NUMBER
        {
          defData->hasDoStep = 1;
          defData->Row.setHasDoStep();
          defData->xStep = $2;
          defData->yStep = $3;
        }

row_options: // empty 
      | row_options row_option
      ;

row_option : '+' K_PROPERTY {defData->dumb_mode = DEF_MAX_INT; }
             row_prop_list
         { defData->dumb_mode = 0; }

row_prop_list : // empty 
       | row_prop_list row_prop
       ;
       
row_prop : T_STRING NUMBER
        {
          if (defData->callbacks->RowCbk) {
             char propTp;
             char* str = defData->ringCopy("                       ");
             propTp =  defData->session->RowProp.propType($1);
             CHKPROPTYPE(propTp, $1, "ROW");
             // For backword compatibility, also set the string value 
             sprintf(str, "%g", $2);
             defData->Row.addNumProperty($1, $2, str, propTp);
          }
        }
      | T_STRING QSTRING
        {
          if (defData->callbacks->RowCbk) {
             char propTp;
             propTp =  defData->session->RowProp.propType($1);
             CHKPROPTYPE(propTp, $1, "ROW");
             defData->Row.addProperty($1, $2, propTp);
          }
        }
      | T_STRING T_STRING
        {
          if (defData->callbacks->RowCbk) {
             char propTp;
             propTp =  defData->session->RowProp.propType($1);
             CHKPROPTYPE(propTp, $1, "ROW");
             defData->Row.addProperty($1, $2, propTp);
          }
        }

tracks_rule: track_start NUMBER
        {
          if (defData->callbacks->TrackCbk) {
            defData->Track.setup($1);
          }
        }
        K_DO NUMBER K_STEP NUMBER track_opts ';' 
        {
          if (($5 <= 0) && (defData->VersionNum >= 5.4)) {
            if (defData->callbacks->TrackCbk)
              if (defData->trackWarnings++ < defData->settings->TrackWarnings) {
                defData->defMsg = (char*)malloc(1000);
                sprintf (defData->defMsg,
                   "The DO number %g in TRACK is invalid.\nThe number value has to be greater than 0. Specify the valid syntax and try again.", $5);
                defData->defError(6525, defData->defMsg);
                free(defData->defMsg);
              }
          }
          if ($7 < 0) {
            if (defData->callbacks->TrackCbk)
              if (defData->trackWarnings++ < defData->settings->TrackWarnings) {
                defData->defMsg = (char*)malloc(1000);
                sprintf (defData->defMsg,
                   "The STEP number %g in TRACK is invalid.\nThe number value has to be greater than 0. Specify the valid syntax and try again.", $7);
                defData->defError(6526, defData->defMsg);
                free(defData->defMsg);
              }
          }
          if (defData->callbacks->TrackCbk) {
            defData->Track.setDo(ROUND($2), ROUND($5), $7);
            CALLBACK(defData->callbacks->TrackCbk, defrTrackCbkType, &defData->Track);
          }
        }

track_start: K_TRACKS track_type
        {
          $$ = $2;
        }

track_type: K_X
            { $$ = (char*)"X";}
        | K_Y
            { $$ = (char*)"Y";}
            
track_opts:
    track_opt_property_statements
    track_mask_statement 
    track_width_statement
    track_ndr_statement
    track_layer_statement
    {}

track_opt_property_statements:
    | track_property_statements
    {}

track_property_statements: 
    track_property_statements track_property_statement
    | track_property_statement
    {}

track_property_statement: K_PROPERTY 
           { 
                defData->dumb_mode = 2; 
           }            
           prop_name_value
           { 
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("TRACKS ... PROPERTY propName propVal")) {
                    CHKERR();
                }
            } else {
                if (defData->callbacks->TrackCbk) {
                    defData->setPropDataType($3, "TRACK", defData->session->TrackProp);
                    defData->Track.addProp($3);
                    $3 = NULL;
                }
            }
            
            delete $3;
           }
            
track_ndr_statement: // empty 
        | K_NDR {defData->dumb_mode=1;} T_STRING
           { 
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("TRACKS ... NDR ruleName")) {
                    CHKERR();
                }
            } else {
                if (defData->callbacks->TrackCbk) {
                    defData->Track.setNdr($3);
                }
            }
           }

track_width_statement: // empty 
        | K_WIDTH NUMBER
           { 
            if (defData->VersionNum < 6.0 - 0.00001) {
                if (defData->def60NewSyntaxError("TRACKS ... WIDTH width")) {
                    CHKERR();
                }
            } else {
                if (defData->callbacks->TrackCbk) {
                    defData->Track.setWidth($2);
                }
            }
           }
            
track_mask_statement: // empty 
        | K_MASK NUMBER same_mask
           { 
              if (defData->validateMaskInput((int)$2, defData->trackWarnings, defData->settings->TrackWarnings)) {
                  if (defData->callbacks->TrackCbk) {
                    defData->Track.addMask($2, $3);
                  }
               }
            }
            
same_mask: 
        // empty 
        { $$ = 0; }
        | K_SAMEMASK
        { $$ = 1; }
        
track_layer_statement: // empty 
        | K_LAYER { defData->dumb_mode = 1000; } track_layer track_layers
            { defData->dumb_mode = 0; }

track_layers: // empty 
        | track_layer track_layers 
            ;

track_layer: T_STRING
        {
          if (defData->callbacks->TrackCbk)
            defData->Track.addLayer($1);
        }

gcellgrid: K_GCELLGRID track_type NUMBER 
     K_DO NUMBER K_STEP NUMBER ';'
        {
          if ($5 <= 0) {
            if (defData->callbacks->GcellGridCbk)
              if (defData->gcellGridWarnings++ < defData->settings->GcellGridWarnings) {
                defData->defMsg = (char*)malloc(1000);
                sprintf (defData->defMsg,
                   "The DO number %g in GCELLGRID is invalid.\nThe number value has to be greater than 0. Specify the valid syntax and try again.", $5);
                defData->defError(6527, defData->defMsg);
                free(defData->defMsg);
              }
          }
          if ($7 < 0) {
            if (defData->callbacks->GcellGridCbk)
              if (defData->gcellGridWarnings++ < defData->settings->GcellGridWarnings) {
                defData->defMsg = (char*)malloc(1000);
                sprintf (defData->defMsg,
                   "The STEP number %g in GCELLGRID is invalid.\nThe number value has to be greater than 0. Specify the valid syntax and try again.", $7);
                defData->defError(6528, defData->defMsg);
                free(defData->defMsg);
              }
          }
          if (defData->callbacks->GcellGridCbk) {
            defData->GcellGrid.setup($2, ROUND($3), ROUND($5), $7);
            CALLBACK(defData->callbacks->GcellGridCbk, defrGcellGridCbkType, &defData->GcellGrid);
          }
        }

extension_section: K_BEGINEXT
        {
            if (defData->VersionNum >= 6.0 - 0.00001) { 
                if (defData->def60ObsoletedError("BEGINEXT ... ENDEXT")) {
                    CHKERR();
                }
            } else if (defData->callbacks->ExtensionCbk) {
                CALLBACK(defData->callbacks->ExtensionCbk, defrExtensionCbkType, &defData->History_text[0]);
            }
        }

extension_stmt: '+' K_BEGINEXT
        { 
            if (defData->VersionNum >= 6.0 - 0.00001) { 
                if (defData->def60ObsoletedError("+ BEGINEXT ... ENDEXT")) {
                    CHKERR();
                }
            }
        }

via_section: via via_declarations via_end
            ;
        
via: K_VIAS NUMBER ';' 
        {
          if (defData->callbacks->ViaStartCbk)
            CALLBACK(defData->callbacks->ViaStartCbk, defrViaStartCbkType, ROUND($2));
        }

via_declarations: // empty 
        | via_declarations via_declaration
            ;

via_declaration: '-' {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING
            {
              if (defData->callbacks->ViaCbk) defData->Via.setup($3);
              defData->viaRule = 0;
            }
        layer_stmts ';'
            {
              if (defData->callbacks->ViaCbk)
                CALLBACK(defData->callbacks->ViaCbk, defrViaCbkType, &defData->Via);
              defData->Via.clear();
            }

layer_stmts: // empty 
        | layer_stmts layer_stmt
            ;

layer_stmt: '+' K_RECT {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING mask pt pt 
        { 
            if (defData->callbacks->ViaCbk)
            if (defData->validateMaskInput($5, defData->viaWarnings, defData->settings->ViaWarnings)) {
                defData->Via.addLayer($4, $6.x, $6.y, $7.x, $7.y, $5);
            }
        }
        | '+' K_PROPERTY 
        { 
            defData->dumb_mode = 2; 
        } 
        prop_name_value
        {
            if (defData->VersionNum < 6.0 - 0.0001) {
                if (defData->def60NewSyntaxError("VIAS ... - viaName + PROPERTY propName propValue")) {
                    CHKERR();
                }
             } else {
                if (defData->callbacks->ViaCbk) {
                    defData->setPropDataType($4, "VIA", defData->session->ViaProp);
                    defData->Via.addProp($4);
                    $4 = NULL;
                }
             }

             delete $4;
        }
        | '+' K_POLYGON { defData->dumb_mode = 1; } T_STRING mask
            {
              if (defData->VersionNum < 5.6) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf (defData->defMsg,
                       "The POLYGON statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                    defData->defError(6509, defData->defMsg);
                    free(defData->defMsg);
                    CHKERR();
                  }
                }
              }
              
              defData->Geometries.Reset();
              
            }
            firstPt nextPt nextPt otherPts
            {
              if (defData->VersionNum >= 5.6) {  // only add if 5.6 or beyond
                if (defData->callbacks->ViaCbk)
                  if (defData->validateMaskInput($5, defData->viaWarnings, defData->settings->ViaWarnings)) {
                    defData->Via.addPolygon($4, &defData->Geometries, $5);
                  }
              }
            }
        | '+' K_PATTERNNAME {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING
            {
              if (defData->VersionNum < 5.6) {
                if (defData->callbacks->ViaCbk)
                  defData->Via.addPattern($4);
              } else
                if (defData->callbacks->ViaCbk)
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings)
                    defData->defWarning(7019, "The PATTERNNAME statement is obsolete in version 5.6 and later.\nThe DEF parser will ignore this statement."); 
            }
        | '+' K_VIARULE {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING
          '+' K_CUTSIZE NUMBER NUMBER
          '+' K_LAYERS {defData->dumb_mode = 3;defData->no_num = 1; } T_STRING T_STRING T_STRING
          '+' K_CUTSPACING NUMBER NUMBER
          '+' K_ENCLOSURE NUMBER NUMBER NUMBER NUMBER
            {
               defData->viaRule = 1;
               if (defData->VersionNum < 5.6) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf (defData->defMsg,
                       "The VIARULE statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                    defData->defError(6557, defData->defMsg);
                    free(defData->defMsg);
                    CHKERR();
                  }
                }
              } else {
                if (defData->callbacks->ViaCbk)
                   defData->Via.addViaRule($4, (int)$7, (int)$8, $12, $13,
                                             $14, (int)$17, (int)$18, (int)$21,
                                             (int)$22, (int)$23, (int)$24); 
              }
            }
        | layer_viarule_opts
        | extension_stmt
          { 
            if (defData->callbacks->ViaExtCbk)
              CALLBACK(defData->callbacks->ViaExtCbk, defrViaExtCbkType, &defData->History_text[0]);
          }

layer_viarule_opts: '+' K_ROWCOL NUMBER NUMBER
            {
              if (!defData->viaRule) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defError(6559, "The ROWCOL statement is missing from the VIARULE statement. Ensure that it exists in the VIARULE statement.");
                    CHKERR();
                  }
                }
              } else if (defData->callbacks->ViaCbk)
                 defData->Via.addRowCol((int)$3, (int)$4);
            }
        | '+' K_ORIGIN NUMBER NUMBER
            {
              if (!defData->viaRule) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defError(6560, "The ORIGIN statement is missing from the VIARULE statement. Ensure that it exists in the VIARULE statement.");
                    CHKERR();
                  }
                }
              } else if (defData->callbacks->ViaCbk)
                 defData->Via.addOrigin((int)$3, (int)$4);
            }
        | '+' K_OFFSET NUMBER NUMBER NUMBER NUMBER
            {
              if (!defData->viaRule) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defError(6561, "The OFFSET statement is missing from the VIARULE statement. Ensure that it exists in the VIARULE statement.");
                    CHKERR();
                  }
                }
              } else if (defData->callbacks->ViaCbk)
                 defData->Via.addOffset((int)$3, (int)$4, (int)$5, (int)$6);
            }
        | '+' K_PATTERN {defData->dumb_mode = 1;defData->no_num = 1; } T_STRING 
            {
              if (!defData->viaRule) {
                if (defData->callbacks->ViaCbk) {
                  if (defData->viaWarnings++ < defData->settings->ViaWarnings) {
                    defData->defError(6562, "The PATTERN statement is missing from the VIARULE statement. Ensure that it exists in the VIARULE statement.");
                    CHKERR();
                  }
                }
              } else if (defData->callbacks->ViaCbk)
                 defData->Via.addCutPattern($4);
            }

firstPt: pt
          { defData->Geometries.startList($1.x, $1.y); }

nextPt: pt
          { defData->Geometries.addToList($1.x, $1.y); }

otherPts: // empty 
        | otherPts nextPt
        ;

pt: '(' NUMBER NUMBER ')'
          {
            defData->save_x = $2;
            defData->save_y = $3;
            $$.x = ROUND($2);
            $$.y = ROUND($3);
          }
        | '(' '*' NUMBER ')'
          {
            defData->save_y = $3;
            $$.x = ROUND(defData->save_x);
            $$.y = ROUND($3);
          }
        | '(' NUMBER '*' ')'
          {
            defData->save_x = $2;
            $$.x = ROUND($2);
            $$.y = ROUND(defData->save_y);
          }
        | '(' '*' '*' ')'
          {
            $$.x = ROUND(defData->save_x);
            $$.y = ROUND(defData->save_y);
          }
          
mask: // empty 
      { $$ = 0; }
      | '+' K_MASK NUMBER
      { $$ = $3; }

via_end: K_END K_VIAS
        { 
          if (defData->callbacks->ViaEndCbk)
            CALLBACK(defData->callbacks->ViaEndCbk, defrViaEndCbkType, 0);
        }

regions_section: regions_start regions_stmts K_END K_REGIONS
        {
          if (defData->callbacks->RegionEndCbk)
            CALLBACK(defData->callbacks->RegionEndCbk, defrRegionEndCbkType, 0);
        }

regions_start: K_REGIONS NUMBER ';'
        {
          if (defData->callbacks->RegionStartCbk)
            CALLBACK(defData->callbacks->RegionStartCbk, defrRegionStartCbkType, ROUND($2));
        }

regions_stmts: // empty 
        | regions_stmts regions_stmt
            {}

regions_stmt: '-' { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        {
          if (defData->callbacks->RegionCbk)
             defData->Region.setup($3);
          defData->regTypeDef = 0;
        }
     rect_list region_options ';'
        { CALLBACK(defData->callbacks->RegionCbk, defrRegionCbkType, &defData->Region); }

rect_list :
      pt pt
        { if (defData->callbacks->RegionCbk)
          defData->Region.addRect($1.x, $1.y, $2.x, $2.y); }
      | rect_list pt pt
        { if (defData->callbacks->RegionCbk)
          defData->Region.addRect($2.x, $2.y, $3.x, $3.y); }
      ;

region_options: // empty 
      | region_options region_option
      ;

region_option : '+' K_PROPERTY {defData->dumb_mode = DEF_MAX_INT; }
                region_prop_list
         { defData->dumb_mode = 0; }
      | '+' K_TYPE region_type      // 5.4.1 
         {
           if (defData->regTypeDef) {
              if (defData->callbacks->RegionCbk) {
                if (defData->regionWarnings++ < defData->settings->RegionWarnings) {
                  defData->defError(6563, "The TYPE statement already exists. It has been defined in the REGION statement.");
                  CHKERR();
                }
              }
           }
           if (defData->callbacks->RegionCbk) defData->Region.setType($3);
           defData->regTypeDef = 1;
         }
      ;

region_prop_list : // empty 
       | region_prop_list region_prop
       ;
       
region_prop : T_STRING NUMBER
        {
          if (defData->callbacks->RegionCbk) {
             char propTp;
             char* str = defData->ringCopy("                       ");
             propTp = defData->session->RegionProp.propType($1);
             CHKPROPTYPE(propTp, $1, "REGION");
             // For backword compatibility, also set the string value 
             // We will use a temporary string to store the number.
             // The string space is borrowed from the ring buffer
             // in the lexer.
             sprintf(str, "%g", $2);
             defData->Region.addNumProperty($1, $2, str, propTp);
          }
        }
      | T_STRING QSTRING
        {
          if (defData->callbacks->RegionCbk) {
             char propTp;
             propTp = defData->session->RegionProp.propType($1);
             CHKPROPTYPE(propTp, $1, "REGION");
             defData->Region.addProperty($1, $2, propTp);
          }
        }
      | T_STRING T_STRING
        {
          if (defData->callbacks->RegionCbk) {
             char propTp;
             propTp = defData->session->RegionProp.propType($1);
             CHKPROPTYPE(propTp, $1, "REGION");
             defData->Region.addProperty($1, $2, propTp);
          }
        }

region_type: K_FENCE
            { $$ = (char*)"FENCE"; }
      | K_GUIDE
            { $$ = (char*)"GUIDE"; }
  
comps_maskShift_section : K_COMPSMASKSHIFT 
         {
           defData->dumb_mode = DEF_MAX_INT; 
           defData->no_num = DEF_MAX_INT;
         }
         layer_statement ';'
         {
           defData->dumb_mode = 0;
           defData->no_num = 0;

           if (defData->VersionNum < 5.8) {
                if (defData->componentWarnings++ < defData->settings->ComponentWarnings) {
                   defData->defMsg = (char*)malloc(10000);
                   sprintf (defData->defMsg,
                     "The MASKSHIFT statement is available in version 5.8 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
                   defData->defError(7415, defData->defMsg);
                   free(defData->defMsg);
                   CHKERR();
                }
            }
            if (defData->callbacks->ComponentMaskShiftLayerCbk) {
                CALLBACK(defData->callbacks->ComponentMaskShiftLayerCbk, defrComponentMaskShiftLayerCbkType, &defData->ComponentMaskShiftLayer);
            }
         }
                 
comps_section: start_comps comps_rule end_comps
            ;
         
start_comps: K_COMPS NUMBER ';'
         {
            defData->Component = new defiComponent(defData);

            if (defData->callbacks->ComponentStartCbk) {
                CALLBACK(defData->callbacks->ComponentStartCbk,
                         defrComponentStartCbkType, ROUND($2));
            }
         }
         
layer_statement : // empty 
         | layer_statement maskLayer
         ;
         
maskLayer: T_STRING
        {
            if (defData->callbacks->ComponentMaskShiftLayerCbk) {
              defData->ComponentMaskShiftLayer.addMaskShiftLayer($1);
            }
        } 

comps_rule: // empty 
        | comps_rule comp
            ;

comp: comp_start comp_options ';'
         {
            if (defData->callbacks->ComponentCbk) {
                CALLBACK(defData->callbacks->ComponentCbk,
                         defrComponentCbkType, defData->Component);

                defData->Component->clear();
            }
         }

comp_start: comp_id_and_name comp_net_list
         {
            defData->dumb_mode = 0;
            defData->no_num = 0;
         }

comp_id_and_name: '-' {defData->dumb_mode = DEF_MAX_INT; defData->no_num = DEF_MAX_INT; }
       T_STRING T_STRING
         {
            if (defData->callbacks->ComponentCbk)
              defData->Component->IdAndName($3, $4);
         }

comp_net_list: // empty 
        { }
        | comp_net_list '*'
            {
              if (defData->callbacks->ComponentCbk)
                defData->Component->addNet("*");
            }
        | comp_net_list T_STRING
            {
              if (defData->callbacks->ComponentCbk)
                defData->Component->addNet($2);
            }
            
comp_options: // empty 
        |     comp_options comp_option
            ;
    
comp_option:  comp_generate | comp_source | comp_type | comp_pinprop | comp_physical | weight | maskShift |
              comp_foreign | comp_region | comp_eeq | comp_halo |
              comp_routehalo | comp_property | comp_extension_stmt
            ;

comp_extension_stmt: extension_stmt
        {
          if (defData->callbacks->ComponentCbk)
            CALLBACK(defData->callbacks->ComponentExtCbk, defrComponentExtCbkType,
                     &defData->History_text[0]);
        }

comp_eeq: '+' K_EEQMASTER {defData->dumb_mode=1; defData->no_num = 1; } T_STRING
        {
          if (defData->callbacks->ComponentCbk)
            defData->Component->setEEQ($4);
        }

comp_generate: '+' K_COMP_GEN { defData->dumb_mode = 2;  defData->no_num = 2; } T_STRING
    opt_pattern
        {
          if (defData->callbacks->ComponentCbk)
             defData->Component->setGenerate($4, $5);
        }
opt_pattern :
    // empty 
      { $$ = (char*)""; }
    | T_STRING
      { $$ = $1; }

comp_source: '+' K_SOURCE source_type 
        {
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("COMPONENTS ... + SOURCE DIST|NETLIST|TEST|TIMING|USER")) {
                    CHKERR();
                }
            } else if (defData->callbacks->ComponentCbk) {
                defData->Component->setSource($3);
            }
        }

source_type: K_NETLIST
            { $$ = (char*)"NETLIST"; }
        | K_DIST
            { $$ = (char*)"DIST"; }
        | K_USER
            { $$ = (char*)"USER"; }
        | K_TIMING
            { $$ = (char*)"TIMING"; }


comp_region:
        comp_region_start comp_pnt_list
        { }
        | comp_region_start T_STRING 
        {
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("COMPONENTS ... + REGION regionName")) {
                    CHKERR();
                }
            } else if (defData->callbacks->ComponentCbk) {
                defData->Component->setRegionName($2);
            }
        }

comp_pnt_list: pt pt
        { 
          // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
          if (defData->VersionNum < 5.5) {
            if (defData->callbacks->ComponentCbk)
               defData->Component->setRegionBounds($1.x, $1.y, 
                                                            $2.x, $2.y);
          }
          else
            defData->defWarning(7020, "The REGION pt pt statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
        } 
    | comp_pnt_list pt pt
        { 
          // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
          if (defData->VersionNum < 5.5) {
            if (defData->callbacks->ComponentCbk)
               defData->Component->setRegionBounds($2.x, $2.y,
                                                            $3.x, $3.y);
          }
          else
            defData->defWarning(7020, "The REGION pt pt statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
        } 

comp_halo: '+' K_HALO                    // 5.7 
        {
          if (defData->VersionNum < 5.6) {
             if (defData->callbacks->ComponentCbk) {
               if (defData->componentWarnings++ < defData->settings->ComponentWarnings) {
                 defData->defMsg = (char*)malloc(1000);
                 sprintf (defData->defMsg,
                    "The HALO statement is a version 5.6 and later syntax.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                 defData->defError(6529, defData->defMsg);
                 free(defData->defMsg);
                 CHKERR();
               }
             }
          }
        }
        halo_soft NUMBER NUMBER NUMBER NUMBER 
        {
          if (defData->callbacks->ComponentCbk)
            defData->Component->setHalo((int)$5, (int)$6,
                                                 (int)$7, (int)$8);
        }

halo_soft: // 5.7 
    | K_SOFT
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->ComponentCbk) {
             if (defData->componentWarnings++ < defData->settings->ComponentWarnings) {
                defData->defMsg = (char*)malloc(10000);
                sprintf (defData->defMsg,
                  "The HALO SOFT is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                defData->defError(6550, defData->defMsg);
                free(defData->defMsg);
                CHKERR();
             }
           }
        } else {
           if (defData->callbacks->ComponentCbk)
             defData->Component->setHaloSoft();
        }
      }

// 5.7 
comp_routehalo: '+' K_ROUTEHALO NUMBER { defData->dumb_mode = 2; defData->no_num = 2; } T_STRING T_STRING
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->ComponentCbk) {
             if (defData->componentWarnings++ < defData->settings->ComponentWarnings) {
                defData->defMsg = (char*)malloc(10000);
                sprintf (defData->defMsg,
                  "The ROUTEHALO is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                defData->defError(6551, defData->defMsg);
                free(defData->defMsg);
                CHKERR();
             }
           }
        } else {
           if (defData->callbacks->ComponentCbk)
             defData->Component->setRouteHalo(
                            (int)$3, $5, $6);
        }
      }

comp_property: '+' K_PROPERTY { defData->dumb_mode = DEF_MAX_INT; }
      comp_prop_list
      { defData->dumb_mode = 0; }

comp_prop_list: comp_prop
    | comp_prop_list comp_prop
          ;

comp_prop: T_STRING NUMBER
        {
          if (defData->callbacks->ComponentCbk) {
            char propTp;
            char* str = defData->ringCopy("                       ");
            propTp = defData->session->CompProp.propType($1);
            CHKPROPTYPE(propTp, $1, "COMPONENT");
            sprintf(str, "%g", $2);
            defData->Component->addNumProperty($1, $2, str, propTp);
          }
        }
     | T_STRING QSTRING
        {
          if (defData->callbacks->ComponentCbk) {
            char propTp;
            propTp = defData->session->CompProp.propType($1);
            CHKPROPTYPE(propTp, $1, "COMPONENT");
            defData->Component->addProperty($1, $2, propTp);
          }
        }
     | T_STRING T_STRING
        {
          if (defData->callbacks->ComponentCbk) {
            char propTp;
            propTp = defData->session->CompProp.propType($1);
            CHKPROPTYPE(propTp, $1, "COMPONENT");
            defData->Component->addProperty($1, $2, propTp);
          }
        }

comp_region_start: '+' K_REGION
        { defData->dumb_mode = 1; defData->no_num = 1; }

comp_foreign: '+' K_FOREIGN { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        opt_paren orient
        { 
          if (defData->VersionNum < 5.6) {
            if (defData->callbacks->ComponentCbk) {
              defData->Component->setForeignName($4);
              defData->Component->setForeignLocation($5.x, $5.y, $6);
            }
         } else
            if (defData->callbacks->ComponentCbk)
              if (defData->componentWarnings++ < defData->settings->ComponentWarnings)
                defData->defWarning(7021, "The FOREIGN statement is obsolete in version 5.6 and later.\nThe DEF parser will ignore this statement.");
         }

opt_paren:
       pt
         { $$ = $1; }
       | NUMBER NUMBER
         { $$.x = ROUND($1); $$.y = ROUND($2); }

comp_type: placement_status pt orient
        {
          if (defData->callbacks->ComponentCbk) {
            defData->Component->setPlacementStatus($1);
            defData->Component->setPlacementLocation($2.x, $2.y, $3);
          }
        }
        | '+' K_UNPLACED
        {
          if (defData->callbacks->ComponentCbk)
            defData->Component->setPlacementStatus(
                                         DEFI_COMPONENT_UNPLACED);
            defData->Component->setPlacementLocation(-1, -1, -1);
        }
        | '+' K_UNPLACED pt orient
        {
          if (defData->VersionNum < 5.4) {   // PCR 495463 
            if (defData->callbacks->ComponentCbk) {
              defData->Component->setPlacementStatus(
                                          DEFI_COMPONENT_UNPLACED);
              defData->Component->setPlacementLocation($3.x, $3.y, $4);
            }
          } else {
            if (defData->componentWarnings++ < defData->settings->ComponentWarnings)
               defData->defWarning(7022, "In the COMPONENT UNPLACED statement, point and orient are invalid in version 5.4 and later.\nThe DEF parser will ignore this statement.");
          }
        }

        // Adding 'no_num' modification, otherwise the token will be parsed as number (double). 
maskShift: '+' K_MASKSHIFT { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        {  
          if (defData->callbacks->ComponentCbk) {
            if (defData->validateMaskShiftInput($4, defData->componentWarnings, defData->settings->ComponentWarnings)) {
                defData->Component->setMaskShift($4);
            }
          }
        }
        
placement_status: '+' K_FIXED 
        { $$ = DEFI_COMPONENT_FIXED; }
        | '+' K_COVER 
        { $$ = DEFI_COMPONENT_COVER; }
        | '+' K_PLACED
        { $$ = DEFI_COMPONENT_PLACED; }
        | '+' K_SOFTFIXED
        { 
            if (defData->VersionNum < 6.0 - 0.0001) {
                if (defData->def60NewSyntaxError("COMPONENTS numComps ; - compName modelName SOFTFIXED ...")) {
                    CHKERR();
                }
            
                $$ = DEFI_COMPONENT_UNPLACED; 
            } else {
                $$ = DEFI_COMPONENT_SOFTFIXED; 
            }
        }

comp_pinprop: '+' K_PINPROPERTY {defData->dumb_mode = 3;} T_STRING prop_name_value
        {
          if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("COMPONENTS ... + PINPROPERTY propName propType")) {
                CHKERR();
            }
          }

          if (defData->callbacks->ComponentCbk) {
            defData->setPropDataType($5, "COMPONENTPIN", defData->session->CompPinProp);
            defData->Component->addPinprop($4, $5);
            $5 = 0;
          }

          delete $5;
        }

comp_physical: '+' K_PHYSICAL
        {
          if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("COMPONENTS ... + PHYSICAL")) {
                CHKERR();
            }
          }

          if (defData->callbacks->ComponentCbk) {
            defData->Component->setPhysical();
          }
        }

weight: '+' K_WEIGHT NUMBER 
        {
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("COMPONENTS ...+ WEIGHT weight")) {
                    CHKERR();
                }
            } else if (defData->callbacks->ComponentCbk) {
                defData->Component->setWeight(ROUND($3));
            }
        }

end_comps: K_END K_COMPS
        { 
            if (defData->callbacks->ComponentEndCbk) {
                  CALLBACK(defData->callbacks->ComponentEndCbk,
                           defrComponentEndCbkType, 0);
            }

            delete defData->Component;
            defData->Component = NULL;
        }

nets_section:  start_nets net_rules end_nets
        ;

start_nets: K_NETS NUMBER ';'
    {
        defData->Net = new defiNet(defData);

        if (defData->callbacks->NetStartCbk) {
            CALLBACK(defData->callbacks->NetStartCbk,
                     defrNetStartCbkType, ROUND($2));
        }

        defData->netOsnet = 1;
        defData->routeStatus = (char*)"ROUTED";
        defData->shieldName = NULL;
    }

net_rules: // empty 
        | net_rules one_net
            ;

one_net:
    net_and_connections net_options ';'
    { 
        if (defData->callbacks->NetCbk) {
            defData->setPropsDataTypes("NET", defData->session->NetProp);
            defData->addNetProps();
            defData->cleanProps();

            CALLBACK(defData->callbacks->NetCbk, defrNetCbkType, defData->Net);

            defData->Net->clear();
        }
    }
/*
** net_and_connections: net_start {defData->dumb_mode = DEF_MAX_INT; no_num = DEF_MAX_INT;}
**                      net_connections
** wmd -- this can be used to replace
**        | '(' K_PIN {defData->dumb_mode = 1; no_num = 1;} T_STRING conn_opt ')' (???)
*/
net_and_connections: net_start
        {defData->dumb_mode = 0; defData->no_num = 0; }

/* pcr 235555 & 236210 */
net_start: '-' 
        {
            defData->dumb_mode = DEF_MAX_INT; 
            defData->no_num = DEF_MAX_INT; 
            defData->nondef_is_keyword = TRUE; 
            defData->mustjoin_is_keyword = TRUE;
            defData->routeStatus = (char*)"ROUTED";
            defData->shieldName = NULL;
        } net_name 

net_name: T_STRING
        {
          // 9/22/1999 
          // this is shared by both net and special net 
          if ((defData->callbacks->NetCbk && (defData->netOsnet==1)) || (defData->callbacks->SNetCbk && (defData->netOsnet==2)))
            defData->Net->setName($1);
          if (defData->callbacks->NetNameCbk)
            CALLBACK(defData->callbacks->NetNameCbk, defrNetNameCbkType, $1);
        } net_connections
        | K_MUSTJOIN '(' T_STRING {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING ')'
        {
          if ((defData->callbacks->NetCbk && (defData->netOsnet==1)) || (defData->callbacks->SNetCbk && (defData->netOsnet==2)))
            defData->Net->addMustPin($3, $5, 0);
          defData->dumb_mode = 3;
          defData->no_num = 3;
        }

net_connections: // empty 
        | net_connections net_connection 
        ;

net_connection: '(' T_STRING {defData->dumb_mode = DEF_MAX_INT; defData->no_num = DEF_MAX_INT;}
                T_STRING conn_opt ')'
        {
          // 9/22/1999 
          // since the code is shared by both net & special net, 
          // need to check on both flags 
          if ((defData->callbacks->NetCbk && (defData->netOsnet==1)) || (defData->callbacks->SNetCbk && (defData->netOsnet==2)))
            defData->Net->addPin($2, $4, $5);
          // 1/14/2000 - pcr 289156 
          // reset defData->dumb_mode & defData->no_num to 3 , just in case 
          // the next statement is another net_connection 
          defData->dumb_mode = 3;
          defData->no_num = 3;
        }
        | '(' '*' {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING conn_opt ')'
        {
          if ((defData->callbacks->NetCbk && (defData->netOsnet==1)) || (defData->callbacks->SNetCbk && (defData->netOsnet==2)))
            defData->Net->addPin("*", $4, $5);
          defData->dumb_mode = 3;
          defData->no_num = 3;
        }
        | '(' K_PIN {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING conn_opt ')'
        {
          if ((defData->callbacks->NetCbk && (defData->netOsnet==1)) || (defData->callbacks->SNetCbk && (defData->netOsnet==2)))
            defData->Net->addPin("PIN", $4, $5);
          defData->dumb_mode = 3;
          defData->no_num = 3;
        }

conn_opt: // empty 
          { $$ = 0; }
        | extension_stmt
        {
          if (defData->callbacks->NetConnectionExtCbk)
            CALLBACK(defData->callbacks->NetConnectionExtCbk, defrNetConnectionExtCbkType,
              &defData->History_text[0]);
          $$ = 0;
        }
        | '+' K_SYNTHESIZED
        {  
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... {compName pinName | PIN pinName} + SYNTHESIZED")) {
                    CHKERR();
                }
            } 
           
            $$ = 1; 
        }
        
        
// These are all the optional fields for a net that go after the '+' 
net_options: // empty 
        | net_options net_option
        ;

net_option: '+' net_type
        {
            if (defData->callbacks->NetCbk) {
                defData->setPropsDataTypes("NET", defData->session->NetProp);
                defData->addNetProps();
             }

            defData->cleanProps();        
            defData->dumb_mode = 1; 
        }
        opt_wire
        {}    

        | '+' K_SOURCE netsource_type
        { 
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + SOURCE {DIST|NETLIST|TEST|TIMING|USER}")) {
                    CHKERR();
                }
            } else if (defData->callbacks->NetCbk) {
                defData->Net->setSource($3); 
            }
        }

        | '+' K_FIXEDBUMP
        {
          if (defData->VersionNum < 5.5) {
            if (defData->callbacks->NetCbk) {
              if (defData->netWarnings++ < defData->settings->NetWarnings) {
                 defData->defMsg = (char*)malloc(1000);
                 sprintf (defData->defMsg,
                    "The FIXEDBUMP statement is available in version 5.5 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                 defData->defError(6530, defData->defMsg);
                 free(defData->defMsg);
                 CHKERR();
              }
            }
          }
          if (defData->callbacks->NetCbk) defData->Net->setFixedbump();
        } 

        | '+' K_FREQUENCY { defData->real_num = 1; } NUMBER
        {
          if (defData->VersionNum < 5.5) {
            if (defData->callbacks->NetCbk) {
              if (defData->netWarnings++ < defData->settings->NetWarnings) {
                 defData->defMsg = (char*)malloc(1000);
                 sprintf (defData->defMsg,
                    "The FREQUENCY statement is a version 5.5 and later syntax.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
                 defData->defError(6558, defData->defMsg);
                 free(defData->defMsg);
                 CHKERR();
              }
            }
          }
          if (defData->callbacks->NetCbk) defData->Net->setFrequency($4);
          defData->real_num = 0;
        }

        | '+' K_ORIGINAL {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING
        { 
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + ORIGINAL netName")) {
                    CHKERR();
                }
            } else if (defData->callbacks->NetCbk) {
                defData->Net->setOriginal($4); 
            }
        }
        | '+' K_PATTERN nets_pattern_type
        { if (defData->callbacks->NetCbk) defData->Net->setPattern($3); }

        | '+' K_WEIGHT NUMBER
        { if (defData->callbacks->NetCbk) defData->Net->setWeight(ROUND($3)); }

        | '+' K_XTALK NUMBER
        { 
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + XTALK class")) {
                    CHKERR();
                }
            } else if (defData->callbacks->NetCbk) {
                defData->Net->setXTalk(ROUND($3)); 
            }
        }

        | '+' K_ESTCAP NUMBER
        {  
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + ESTCAP wireCapacitance")) {
                    CHKERR();
                }
            } else if (defData->callbacks->NetCbk) {
                defData->Net->setCap($3); 
            }
         }

        | '+' K_USE use_type 
        { if (defData->callbacks->NetCbk) defData->Net->setUse($3); }

        | '+' K_STYLE NUMBER
        { if (defData->callbacks->NetCbk) defData->Net->setStyle((int)$3); }

        | '+' K_NONDEFAULTRULE { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        { 
          if (defData->callbacks->NetCbk && defData->callbacks->NetNonDefaultRuleCbk) {
             // User wants a callback on nondefaultrule 
             CALLBACK(defData->callbacks->NetNonDefaultRuleCbk,
                      defrNetNonDefaultRuleCbkType, $4);
          }
          // Still save data in the class 
          if (defData->callbacks->NetCbk) defData->Net->setNonDefaultRule($4);
        }

        | vpin_stmt

        | '+' K_SHIELDNET { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        { if (defData->callbacks->NetCbk) defData->Net->addShieldNet($4); }

        {
          if (defData->VersionNum < 5.4) {   // PCR 445209 
            defData->by_is_keyword = FALSE;
            defData->do_is_keyword = FALSE;
            defData->new_is_keyword = FALSE;
            defData->step_is_keyword = FALSE;
            defData->nondef_is_keyword = FALSE;
            defData->mustjoin_is_keyword = FALSE;
            defData->orient_is_keyword = FALSE;
            defData->virtual_is_keyword = FALSE;
            defData->rect_is_keyword = FALSE;
            defData->mask_is_keyword = FALSE;
          } else {
            defData->by_is_keyword = FALSE;
            defData->do_is_keyword = FALSE;
            defData->new_is_keyword = FALSE;
            defData->step_is_keyword = FALSE;
            defData->nondef_is_keyword = FALSE;
            defData->mustjoin_is_keyword = FALSE;
            defData->orient_is_keyword = FALSE;
            defData->virtual_is_keyword = FALSE;
            defData->rect_is_keyword = FALSE;
            defData->mask_is_keyword = FALSE;
          }
          defData->needNPCbk = 0;
        }

        | '+' K_SUBNET
        { defData->dumb_mode = 1; defData->no_num = 1;
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + SUBNET")) {
                    CHKERR();
                }
            } 
          
            if (defData->callbacks->NetCbk) {
                defData->Subnet = new defiSubnet(defData);
            }
        }
        T_STRING {
          if (defData->callbacks->NetCbk && defData->callbacks->NetSubnetNameCbk) {
            // User wants a callback on Net subnetName 
            CALLBACK(defData->callbacks->NetSubnetNameCbk, defrNetSubnetNameCbkType, $4);
          }
          // Still save the subnet name in the class 
          if (defData->callbacks->NetCbk) {
            defData->Subnet->setName($4);
          }
        } 
        comp_names {
          defData->routed_is_keyword = TRUE;
          defData->fixed_is_keyword = TRUE;
          defData->cover_is_keyword = TRUE;
        } subnet_options {
          if (defData->callbacks->NetCbk) {
            defData->Net->addSubnet(defData->Subnet);
            defData->Subnet = NULL;
            defData->routed_is_keyword = FALSE;
            defData->fixed_is_keyword = FALSE;
            defData->cover_is_keyword = FALSE;
          }
        }

        | '+' K_PROPERTY 
        {
            defData->dumb_mode = DEF_MAX_INT;
        }
        net_prop_name_values
        {
            defData->dumb_mode = 0;
        }

        | extension_stmt
        { 
          if (defData->callbacks->NetExtCbk)
            CALLBACK(defData->callbacks->NetExtCbk, defrNetExtCbkType, &defData->History_text[0]);
        }

netsource_type: K_NETLIST
        { $$ = (char*)"NETLIST"; }
        | K_DIST
        { $$ = (char*)"DIST"; }
        | K_USER
        { $$ = (char*)"USER"; }
        | K_TIMING
        { $$ = (char*)"TIMING"; }
        | K_TEST
        { $$ = (char*)"TEST"; }

vpin_stmt: vpin_begin vpin_layer_opt pt pt 
        {
          // vpin_options may have to deal with orient 
          defData->orient_is_keyword = TRUE;
        }
        vpin_options
        { if (defData->callbacks->NetCbk)
            defData->Net->addVpinBounds($3.x, $3.y, $4.x, $4.y);
          defData->orient_is_keyword = FALSE;
        }

vpin_begin: '+' K_VPIN {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING
        { 
            if (defData->VersionNum >= 6.0 - 0.00001) {
                if (defData->def60ObsoletedError("NETS ... netName ... + VPIN")) {
                    CHKERR();
                }
            } 
            
            if (defData->callbacks->NetCbk) {
                defData->Net->addVpin($4); 
            }
         }

vpin_layer_opt: // empty 
        | K_LAYER {defData->dumb_mode=1;} T_STRING
        { if (defData->callbacks->NetCbk) defData->Net->addVpinLayer($3); }

vpin_options: // empty 
        | vpin_status pt orient
        { if (defData->callbacks->NetCbk) defData->Net->addVpinLoc($1, $2.x, $2.y, $3); }

vpin_status: K_PLACED 
        { $$ = (char*)"PLACED"; }
        | K_FIXED 
        { $$ = (char*)"FIXED"; }
        | K_COVER
        { $$ = (char*)"COVER"; }

net_type: K_FIXED
        { 
            defData->routeStatus = (char*)"FIXED";
            defData->shieldName = NULL;
        }
        | K_COVER
        { 
            defData->routeStatus = (char*)"COVER";
            defData->shieldName = NULL;
        }
        | K_ROUTED
        { 
            defData->routeStatus = (char*)"ROUTED"; 
            defData->shieldName = NULL;
        }
        | K_NOSHIELD
        {
            if (defData->VersionNum >= 6.0 - 0.00001) {            
                if (defData->def60ObsoletedError("NETS ... regularWiring ... + NOSHIELD")) {
                    CHKERR();
                }
            }

            defData->routeStatus = (char*)"NOSHIELD";
            defData->shieldName = NULL;
        }


opt_wire: 
    {
        if (defData->callbacks->NetCbk) {
            if (defData->VersionNum < 5.4
                && strcmp(defData->routeStatus, "NOSHIELD") == 0) { // PCR445209
                defData->Shield = new defiShield(defData);
                defData->Shield->Init("");
                defData->Net->addShield(defData->Shield, true);
            } else {
                defData->Wire = new defiWire(defData);
                defData->Wire->Init(defData->routeStatus, NULL);
                defData->Net->addWire(defData->Wire);
            }
        }
    }
    opt_paths
    {
        defData->Shield = NULL;
        defData->Wire = NULL;
    }

opt_paths:
    | paths
      {
          defData->by_is_keyword = FALSE;
          defData->do_is_keyword = FALSE;
          defData->new_is_keyword = FALSE;
          defData->nondef_is_keyword = FALSE;
          defData->mustjoin_is_keyword = FALSE;
          defData->step_is_keyword = FALSE;
          defData->orient_is_keyword = FALSE;
          defData->virtual_is_keyword = FALSE;
          defData->rect_is_keyword = FALSE;
          defData->mask_is_keyword = FALSE;
          defData->width_is_keyword = FALSE;
          defData->needNPCbk = 0;
          defData->routeStatus = (char*)"ROUTED";
          defData->shieldName = NULL;
      }

paths:
    {
        if (defData->callbacks->NetCbk) {
            defData->PathObj = new defiPath(defData);
            defData->startPath();
        }
    }
    path   // not necessary to do partial callback for net yet
    {
        if (defData->callbacks->NetCbk) {
            defData->finishPath(0, &defData->needNPCbk);
            defData->PathObj = NULL;
        }
    }
  | paths new_path
    { }

new_path: K_NEW
    {
        defData->dumb_mode = 1;

        if (defData->callbacks->NetCbk) {
            defData->PathObj = new defiPath(defData);
            defData->startPath();
        }
    }
    path
    {
        if (defData->callbacks->NetCbk) {
            defData->finishPath(0, &defData->needNPCbk);
            defData->PathObj = NULL;
        }
    }

path: 
      T_STRING
      {
        if ((strcmp($1, "TAPER") == 0) || (strcmp($1, "TAPERRULE") == 0)) {
          if (defData->callbacks->NetCbk) {
            if (defData->netWarnings++ < defData->settings->NetWarnings) {
              defData->defError(6531, "The layerName which is required in path is missing. Include the layerName in the path and then try again.");
              CHKERR();
            }
          }
          // Since there is already error, the next token is insignificant 
          defData->dumb_mode = 1; defData->no_num = 1;
        } else {
          // CCR 766289 - Do not accummulate the layer information if there 
          // is not a callback set 
          if (defData->callbacks->NetCbk) {
              defData->PathObj->addLayer($1);
          }

          defData->dumb_mode = 0; defData->no_num = 0;
        }
      }
    opt_taper_style_s  path_pt
      { defData->dumb_mode = DEF_MAX_INT; defData->by_is_keyword = TRUE; defData->do_is_keyword = TRUE;
/*
       dumb_mode = 1; by_is_keyword = TRUE; do_is_keyword = TRUE;
*/
        defData->new_is_keyword = TRUE; 
        defData->step_is_keyword = TRUE; 
        defData->orient_is_keyword = TRUE; 
        defData->virtual_is_keyword = TRUE;
        defData->mask_is_keyword = TRUE, 
        defData->rect_is_keyword = TRUE;
        defData->width_is_keyword = TRUE;
      }
    
       path_item_list_opt
     
      { defData->dumb_mode = 0;   defData->virtual_is_keyword = FALSE; defData->mask_is_keyword = FALSE,
       defData->rect_is_keyword = FALSE; }
    
virtual_statement :
    K_VIRTUAL virtual_pt
    {
      if (defData->VersionNum < 5.8) {
              if (defData->callbacks->SNetCbk) {
                if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The VIRTUAL statement is available in version 5.8 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
                  defData->defError(6536, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
          }
    } 
  
rect_statement : 
    K_RECT rect_pts
    {
      if (defData->VersionNum < 5.8) {
              if (defData->callbacks->SNetCbk) {
                if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
                  defData->defMsg = (char*)malloc(1000);
                  sprintf (defData->defMsg,
                     "The RECT statement is available in version 5.8 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
                  defData->defError(6536, defData->defMsg);
                  free(defData->defMsg);
                  CHKERR();
                }
              }
      }
    } 
              

path_item_list_opt: 
    {
    }
    | path_item_list_opt path_item
    {}


path_item:
    T_STRING
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            if (strcmp($1, "TAPER") == 0) {
                defData->PathObj->setTaper();
            } else {
                defData->PathObj->addVia($1);
            }
        }
    }
  | K_MASK NUMBER T_STRING
    {
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                if (strcmp($3, "TAPER") == 0) {
                    defData->PathObj->setTaper();
                } else {
                    defData->PathObj->addViaMask($2);
                    defData->PathObj->addVia($3);
                }
            }
        }
    }
  | T_STRING orient
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addVia($1);
            defData->PathObj->addViaRotation($2);
        }
    }
  | K_MASK NUMBER T_STRING orient
    { 
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                defData->PathObj->addViaMask($2);
                defData->PathObj->addVia($3);
                defData->PathObj->addViaRotation($4);
            }
        }
    }
  | K_MASK NUMBER T_STRING K_DO NUMBER K_BY NUMBER K_STEP NUMBER NUMBER
    {
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {      
            if (($5 == 0 || $7 == 0)
                && defData->callbacks->SNetCbk
                && (defData->netWarnings++ < defData->settings->NetWarnings)) {
                    defData->defError(6533, "Either the numX or numY in the VIA DO statement has the value. The value specified is 0.\nUpdate your DEF file with the correct value and then try again.\n");
                    CHKERR();
            }

            if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
                defData->PathObj->addViaMask($2);
                defData->PathObj->addVia($3);
                defData->PathObj->addViaData((int)$5, (int)$7,
                                             (int)$9, (int)$10);
            } else if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
                if (defData->netWarnings++ < defData->settings->NetWarnings) {
                    defData->defError(6567, "The VIA DO statement is defined in the NET statement and is invalid.\nRemove this statement from your DEF file and try again.");
                    CHKERR();
                }
            }
        }
    }
  | T_STRING K_DO NUMBER K_BY NUMBER K_STEP NUMBER NUMBER
    {
        if ((defData->VersionNum < 5.5)
            && defData->callbacks->SNetCbk
            && (defData->netWarnings++ < defData->settings->NetWarnings)) {
            defData->defMsg = (char*)malloc(1000);
            sprintf (defData->defMsg,
                     "The VIA DO statement is available in version 5.5 and later.\nHowever, your DEF file is defined with version %.2f",
                     defData->VersionNum);
            defData->defError(6532, defData->defMsg);
            free(defData->defMsg);
            CHKERR();
        }

        if (($3 == 0 || $5 == 0)
            && defData->callbacks->SNetCbk
            && (defData->netWarnings++ < defData->settings->NetWarnings)) {
            defData->defError(6533, "Either the numX or numY in the VIA DO statement has the value. The value specified is 0.\nUpdate your DEF file with the correct value and then try again.\n");
            CHKERR();
        }

        if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
            defData->PathObj->addVia($1);
            defData->PathObj->addViaData((int)$3, (int)$5, (int)$7, (int)$8);
        } else if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
            if (defData->netWarnings++ < defData->settings->NetWarnings) {
                defData->defError(6567, "The VIA DO statement is defined in the NET statement and is invalid.\nRemove this statement from your DEF file and try again.");
                CHKERR();
            }
        }
    }
  | T_STRING orient K_DO NUMBER K_BY NUMBER K_STEP NUMBER NUMBER
    {
        if ((defData->VersionNum < 5.5)
            && defData->callbacks->SNetCbk
            && (defData->netWarnings++ < defData->settings->NetWarnings)) {
            defData->defMsg = (char*)malloc(1000);
            sprintf (defData->defMsg, "The VIA DO statement is available in version 5.5 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
            defData->defError(6532, defData->defMsg);
            CHKERR();
        }

        if (($4 == 0 || $6 == 0)
            && defData->callbacks->SNetCbk
            && (defData->netWarnings++ < defData->settings->NetWarnings)) {
            defData->defError(6533, "Either the numX or numY in the VIA DO statement has the value. The value specified is 0.\nUpdate your DEF file with the correct value and then try again.\n");
            CHKERR();
        }

        if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
            defData->PathObj->addVia($1);
            defData->PathObj->addViaRotation($2);
            defData->PathObj->addViaData((int)$4, (int)$6, (int)$8, (int)$9);
        } else if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
            if (defData->netWarnings++ < defData->settings->NetWarnings) {
                defData->defError(6567, "The VIA DO statement is defined in the NET statement and is invalid.\nRemove this statement from your DEF file and try again.");
                CHKERR();
            }
        }
    }
  | K_MASK NUMBER T_STRING orient K_DO NUMBER K_BY NUMBER K_STEP NUMBER NUMBER
    {
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {
            if (($6 == 0 || $8 == 0)
                && defData->callbacks->SNetCbk
                && (defData->netWarnings++ < defData->settings->NetWarnings)) {
                defData->defError(6533, "Either the numX or numY in the VIA DO statement has the value. The value specified is 0.\nUpdate your DEF file with the correct value and then try again.\n");
                CHKERR();
            }

            if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
                defData->PathObj->addViaMask($2); 
                defData->PathObj->addVia($3);
                defData->PathObj->addViaRotation($4);;
                defData->PathObj->addViaData((int)$6, (int)$8, (int)$10, (int)$11);
            } else if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
                if (defData->netWarnings++ < defData->settings->NetWarnings) {
                    defData->defError(6567, "The VIA DO statement is defined in the NET statement and is invalid.\nRemove this statement from your DEF file and try again.");
                    CHKERR();
                }
            }
        }
    }
  | virtual_statement
  | rect_statement 
  | K_MASK NUMBER K_RECT
    {
        defData->dumb_mode = 6;
    }
    '(' NUMBER NUMBER NUMBER NUMBER ')'
    {
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                defData->PathObj->addMask($2);
                defData->PathObj->addViaRect($6, $7, $8, $9);
            }
        }
    }
  | mask_number wire_width path_pt
  | wire_width path_pt 
    {
       // reset defData->dumb_mode to 1 just incase the next token is a via of the path
        // 2/5/2004 - pcr 686781
        defData->dumb_mode = DEF_MAX_INT; defData->by_is_keyword = TRUE; defData->do_is_keyword = TRUE;
        defData->new_is_keyword = TRUE; defData->step_is_keyword = TRUE;
        defData->orient_is_keyword = TRUE;
    }

mask_number:
    K_MASK NUMBER
    {
        if (defData->validateMaskInput((int)$2, defData->sNetWarnings,
                                       defData->settings->SNetWarnings)) {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                defData->PathObj->addMask($2); 
            }
        }  
    }

wire_width:
  | K_WIDTH NUMBER
    {
        if (defData->VersionNum < 6.0 - 0.0001) {
            if (defData->def60NewSyntaxError("NETS ... ( x y [extValue] ) [MASK maskNum] [WIDTH width] ( x y [extValue] )")) {
                CHKERR();
            }
        } else if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
            defData->PathObj->addWidth($2);
        } 
    }
 
path_pt :
    '(' NUMBER NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addPoint(ROUND($2), ROUND($3));
        }

        defData->save_x = $2;
        defData->save_y = $3; 
    }
  | '(' '*' NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addPoint(ROUND(defData->save_x), ROUND($3));
        }

        defData->save_y = $3;
      }
  | '(' NUMBER '*' ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet==1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet==2))) {
            defData->PathObj->addPoint(ROUND($2), ROUND(defData->save_y));
        }

        defData->save_x = $2;
    }
  | '(' '*' '*' ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addPoint(ROUND(defData->save_x),
                                       ROUND(defData->save_y));
        }
    }
  | '(' NUMBER NUMBER NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addFlushPoint(ROUND($2), ROUND($3), ROUND($4));
        }

        defData->save_x = $2;
        defData->save_y = $3;
    }
  | '(' '*' NUMBER NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addFlushPoint(ROUND(defData->save_x),
                                            ROUND($3), ROUND($4));
        }

        defData->save_y = $3;
    }
  | '(' NUMBER '*' NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addFlushPoint(ROUND($2), ROUND(defData->save_y),
                                           ROUND($4));
        }

        defData->save_x = $2;
    }
  | '(' '*' '*' NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet==1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet==2))) {
            defData->PathObj->addFlushPoint(ROUND(defData->save_x),
                                           ROUND(defData->save_y),
                                           ROUND($4));
        }
    }

virtual_pt :
    '(' NUMBER NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addVirtualPoint(ROUND($2), ROUND($3));
        }

        defData->save_x = $2;
        defData->save_y = $3;
    }
  | '(' '*' NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addVirtualPoint(ROUND(defData->save_x), ROUND($3));
        }

        defData->save_y = $3;
    }
  | '(' NUMBER '*' ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addVirtualPoint(ROUND($2), ROUND(defData->save_y));
        }

        defData->save_x = $2;
    }
  | '(' '*' '*' ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
             || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addVirtualPoint(ROUND(defData->save_x),
                                              ROUND(defData->save_y));
        }
    }
 
rect_pts :
    '(' NUMBER NUMBER NUMBER NUMBER ')'
    {
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addViaRect($2, $3, $4, $5); 
        }    
    }   
    
                
opt_taper_style_s: // empty 
    | opt_taper_style_s opt_taper_style

    ;
opt_taper_style: opt_style
    | opt_taper | opt_prop | opt_shield
    ;

opt_prop:
    K_PROPERTY 
    { 
        defData->dumb_mode = 2; 
    } 
    prop_name_value
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("NETS regularWiring: layerName + PROPERTY propName propValue ... routingPoints")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
                defData->setPropDataType($3, "ROUTE",
                                         defData->session->RouteProp);
                defData->PathObj->addProp($3);
                $3 = NULL;
            }
        }

        delete $3;
    }

opt_shield: K_SHIELD
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("NETS regularWiring: layerName + SHIELD ... routingPoints")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->NetCbk && (defData->netOsnet == 1)) {
                defData->PathObj->setShield("");
            }
        }
    }
opt_taper:
    K_TAPER
    {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("NETS ... layerName ... [TAPER]")) {
                CHKERR();
            }
        } else if ((defData->callbacks->NetCbk && (defData->netOsnet==1))
                   || (defData->callbacks->SNetCbk && (defData->netOsnet==2))) {
            defData->PathObj->setTaper(); 
        }
    }
  | K_TAPERRULE { defData->dumb_mode = 1; } T_STRING
    {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("NETS ... layerName ... [TAPERRULE ruleName]")) {
                CHKERR();
            }
        } else if ((defData->callbacks->NetCbk && (defData->netOsnet==1))
                   || (defData->callbacks->SNetCbk && (defData->netOsnet==2))) {
            defData->PathObj->addTaperRule($3); 
        }
    }

opt_style: K_STYLE NUMBER
    { 
        if (defData->VersionNum < 5.6) {
            if (defData->callbacks->NetCbk || defData->callbacks->SNetCbk) {
                if (defData->netWarnings++ < defData->settings->NetWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf (defData->defMsg,
                             "The STYLE statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f",
                             defData->VersionNum);
                    defData->defError(6534, defData->defMsg);
                    free(defData->defMsg);
                    CHKERR();
                }
            }
        } else if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("NETS ... layerName ... [STYLE styleNum]")) {
                CHKERR();
            }
        } else if ((defData->callbacks->NetCbk && (defData->netOsnet==1))
                   || (defData->callbacks->SNetCbk && (defData->netOsnet==2))) {
            defData->PathObj->addStyle((int)$2);
        }
    }

opt_spaths: // empty 
    | opt_spaths opt_shape_style_prop
    ;

opt_shape_style_prop:
    '+' K_SHAPE shape_type
    {  
        if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
            || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
            defData->PathObj->addShape($3); 
        }
    }

  | '+' K_STYLE NUMBER
    {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("SPECIALNETS specialWiring: layerName routeWidth ... + STYLE styleType")) {
                CHKERR();
            }
        } else if (defData->VersionNum < 5.6) {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                if (defData->netWarnings++ < defData->settings->NetWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf (defData->defMsg,
                             "The STYLE statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f",
                             defData->VersionNum);
                    defData->defError(6534, defData->defMsg);
                    free(defData->defMsg);
                    CHKERR();
                }
            }
        } else {
            if ((defData->callbacks->NetCbk && (defData->netOsnet == 1))
                || (defData->callbacks->SNetCbk && (defData->netOsnet == 2))) {
                defData->PathObj->addStyle((int)$3);
            }
        }
    }

  | '+' K_PROPERTY 
    { 
        defData->dumb_mode = 2; 
    }       
    prop_name_value
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("SPECIALNETS specialWiring: layerName routeWidth + PROPERTY propName propValue ... routingPoints")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
                defData->setPropDataType($4, "SPECIALROUTE",
                                         defData->session->SpecialRouteProp);
                defData->PathObj->addProp($4);
                $4 = NULL;
            }
        }
        
        delete $4;
    }

  | '+' K_SHIELD 
    { 
        defData->dumb_mode = 1; 
        defData->no_num = 1; 
    }           
    T_STRING
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("SPECIALNETS specialWiring: layerName routeWidth + SHIELD shieldNetName ... routingPoints")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->SNetCbk && (defData->netOsnet == 2)) {
                defData->PathObj->setShield($4);
            }
        }    
    }

end_nets: K_END K_NETS 
    {
        if (defData->callbacks->NetEndCbk) {
            CALLBACK(defData->callbacks->NetEndCbk, defrNetEndCbkType, 0);
        }

        defData->netOsnet = 0;
        defData->width_is_keyword = FALSE;

        delete defData->Net;
        defData->Net = NULL;
    }

shape_type: K_RING
            { $$ = (char*)"RING"; }
        | K_STRIPE
            { $$ = (char*)"STRIPE"; }
        | K_FOLLOWPIN
            { $$ = (char*)"FOLLOWPIN"; }
        | K_IOWIRE
            { $$ = (char*)"IOWIRE"; }
        | K_COREWIRE
            { $$ = (char*)"COREWIRE"; }
        | K_BLOCKWIRE
            { $$ = (char*)"BLOCKWIRE"; }
        | K_FILLWIRE
            { $$ = (char*)"FILLWIRE"; }
        | K_FILLWIREOPC                         // 5.7 
            {
              if (defData->VersionNum < 5.7) {
                 if (defData->callbacks->NetCbk || defData->callbacks->SNetCbk) {
                   if (defData->fillWarnings++ < defData->settings->FillWarnings) {
                     defData->defMsg = (char*)malloc(10000);
                     sprintf (defData->defMsg,
                       "The FILLWIREOPC is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                     defData->defError(6552, defData->defMsg);
                     free(defData->defMsg);
                     CHKERR();
                  }
                }
              }
              $$ = (char*)"FILLWIREOPC";
            }
        | K_DRCFILL
            { $$ = (char*)"DRCFILL"; }
        | K_BLOCKAGEWIRE
            { 
                if (defData->VersionNum >= 6.0 - 0.00001) {
                    if (defData->def60ObsoletedError("SPECIALNETS ... + SHAPE BLOCKAGEWIRE")) {
                        CHKERR();
                    }
                }
                
                $$ = (char*)"BLOCKAGEWIRE"; 
            }
        | K_PADRING
            { $$ = (char*)"PADRING"; }
        | K_BLOCKRING
            { $$ = (char*)"BLOCKRING"; }

snets_section :  start_snets snet_rules end_snets
            ;

snet_rules: // empty 
        | snet_rules snet_rule
            ;

snet_rule:
    net_and_connections snet_options ';'
    { 
        defData->routeStatus = (char*)"ROUTED";
        defData->shieldName = NULL;

        if (defData->callbacks->SNetCbk) {
            defData->setPropsDataTypes("SPECIAL NET",
                                       defData->session->SNetProp);
            defData->addNetProps();

            CALLBACK(defData->callbacks->SNetCbk,
                     defrSNetCbkType, defData->Net);

            defData->Net->clear();
        }

        defData->cleanProps();
     }

snet_options: // empty 
        | snet_options snet_option
        {
        }

snet_option: snet_width | snet_voltage | 
             snet_spacing | snet_other_option ;

snet_other_option:
    '+' snet_type 
    {
        if (defData->callbacks->SNetCbk) {
            defData->setPropsDataTypes("SPECIAL NET", defData->session->SNetProp);
            defData->addNetProps();
         }

        defData->cleanProps();       
    }
     opt_swire
    {
    }

  | '+' K_SHAPE shape_type
    {  
      defData->shapeType = $3;
    }

  | '+' K_MASK NUMBER
    {
      if (defData->validateMaskInput((int)$3, defData->sNetWarnings, defData->settings->SNetWarnings)) {
          defData->specialWire_mask = $3;
      }     
    }

  | '+' K_PROPERTY 
    {
      defData->dumb_mode = DEF_MAX_INT;
    }
    net_prop_name_values
    {
      defData->dumb_mode = 0;
    }

  | '+' K_POLYGON { defData->dumb_mode = 1; } T_STRING
    {
      if (defData->VersionNum < 5.6) {
        if (defData->callbacks->SNetCbk) {
          if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
            defData->defMsg = (char*)malloc(1000);
            sprintf (defData->defMsg,
               "The POLYGON statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
            defData->defError(6535, defData->defMsg);
            free(defData->defMsg);
            CHKERR();
          }
        }
      }

      defData->Geometries.Reset();
    }

    firstPt nextPt nextPt otherPts
    {
      defrProps* polyProps = NULL;

      if (defData->VersionNum >= 6.0 - 0.00001) {
          defData->setPropsDataTypes("SPECIALROUTE",
                                     defData->session->SpecialRouteProp);
          polyProps = defData->props;
          defData->props = NULL;
          defData->cleanProps();
      }

      if (defData->VersionNum >= 5.6) {  // only add if 5.6 or beyond
        if (defData->callbacks->SNetCbk) {
          defiNetPoly *poly = new defiNetPoly($4,
                                              &defData->Geometries,
                                              defData->specialWire_mask,
                                              defData->routeStatus,
                                              defData->shieldName,
                                              defData->shapeType,
                                              polyProps);

          defData->cleanProps();
          defData->specialWire_mask = 0;

          // defData->needSNPCbk will indicate that it has reach the max
          // memory and if user has set partialPathCBk, def parser
          // will make the callback.
          // This will improve performance
          // This construct is only in specialnet
          defData->Net->addPolygon(poly, &defData->needSNPCbk);

          if (defData->needSNPCbk && defData->callbacks->SNetPartialPathCbk) {
             CALLBACK(defData->callbacks->SNetPartialPathCbk, defrSNetPartialPathCbkType,
                      defData->Net);
             defData->Net->clearRectPolyNPath();
             defData->Net->clearVia();
          }
        }
      }

      defData->routeStatus = (char*)"ROUTED";
      defData->shapeType = NULL;
    }

  | '+' K_RECT { defData->dumb_mode = 1; } T_STRING pt pt
    {
        defrProps* rectProps = NULL;

        if (defData->VersionNum >= 6.0 - 0.00001) {
            defData->setPropsDataTypes("SPECIALROUTE",
                                       defData->session->SpecialRouteProp);
            rectProps = defData->props;
            defData->props = NULL;
            defData->cleanProps();
        }

        if (defData->VersionNum < 5.6) {
            if (defData->callbacks->SNetCbk) {
                if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
                    defData->defMsg = (char*)malloc(1000);
                    sprintf (defData->defMsg,
                             "The RECT statement is available in version 5.6 "
                             "and later.\nHowever, your DEF file is defined "
                             "with version %.2f",
                             defData->VersionNum);
                    defData->defError(6536, defData->defMsg);
                    free(defData->defMsg);
                    CHKERR();
                }
            }
        }

        if (defData->callbacks->SNetCbk) {
            defiNetRect *rect = new defiNetRect($4,
                                                $5.x,
                                                $5.y,
                                                $6.x,
                                                $6.y,
                                                defData->specialWire_mask,
                                                defData->routeStatus,
                                                defData->shieldName,
                                                defData->shapeType,
                                                rectProps);

            defData->specialWire_mask = 0;

            if (defData->callbacks->RectInSNetCbk) {
                CALLBACK2(defData->callbacks->RectInSNetCbk,
                          defrRectInNetCbkType, rect, defData->Net);

                delete rect;
            } else {
                // defData->needSNPCbk will indicate that it has reach the max
                // memory and if user has set partialPathCBk, def parser
                // will make the callback.
                // This will improve performance
                // This construct is only in specialnet
                defData->Net->addRect(rect, &defData->needSNPCbk);

                if (defData->needSNPCbk
                    && defData->callbacks->SNetPartialPathCbk) {
                    CALLBACK(defData->callbacks->SNetPartialPathCbk,
                             defrSNetPartialPathCbkType,
                             defData->Net);
                    defData->Net->clearRectPolyNPath();
                    defData->Net->clearVia();
                }
            }
        }

        defData->routeStatus = (char*)"ROUTED";
        defData->shapeType = NULL;
    }
  | '+' K_VIA { defData->dumb_mode = 1; } T_STRING orient_pt
    {
      if (defData->VersionNum < 5.8) {
          if (defData->callbacks->SNetCbk) {
            if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
              defData->defMsg = (char*)malloc(1000);
              sprintf (defData->defMsg,
                 "The VIA statement is available in version 5.8 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
              defData->defError(6536, defData->defMsg);
              free(defData->defMsg);
              CHKERR();
            }
          }
      }
    }
    firstPt otherPts
    {
      defrProps* viaProps = NULL;

      if (defData->VersionNum >= 6.0 - 0.00001) {
        defData->setPropsDataTypes("SPECIALROUTE", defData->session->SpecialRouteProp);
        viaProps = defData->props;
        defData->props = NULL;
        defData->cleanProps();
      }

      if (defData->VersionNum >= 5.8 && defData->callbacks->SNetCbk) {
          defiNetVia *via = new defiNetVia($4,
                                           $5, 
                                           &defData->Geometries,
                                           defData->specialWire_mask,
                                           defData->routeStatus,
                                           defData->shieldName,
                                           defData->shapeType,
                                           viaProps);

          defData->specialWire_mask = 0;

          defData->Net->addVia(via, &defData->needSNPCbk);

          if (defData->needSNPCbk && defData->callbacks->SNetPartialPathCbk) {
             CALLBACK(defData->callbacks->SNetPartialPathCbk, defrSNetPartialPathCbkType,
                      defData->Net);
             defData->Net->clearRectPolyNPath();
             defData->Net->clearVia();
          }
        }

        defData->routeStatus = (char*)"ROUTED";
        defData->shapeType = NULL;
    }

  | '+' K_SOURCE source_type
    { 
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("SPECIALNETS ... netName ... + SOURCE DIST|NETLIST|TEST|TIMING|USER")) {
                CHKERR();
            }
        } else if (defData->callbacks->SNetCbk) {
            defData->Net->setSource($3); 
        }
    }

  | '+' K_FIXEDBUMP
    { if (defData->callbacks->SNetCbk) defData->Net->setFixedbump(); }

  | '+' K_FREQUENCY NUMBER
    { if (defData->callbacks->SNetCbk) defData->Net->setFrequency($3); }

  | '+' K_ORIGINAL {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING
    { 
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("SPECIALNETS ... netName ... + ORIGINAL netName")) {
                CHKERR();
            }
        } else if (defData->callbacks->SNetCbk) {
            defData->Net->setOriginal($4); 
        }
    }

  | '+' K_PATTERN snets_pattern_type
    { if (defData->callbacks->SNetCbk) defData->Net->setPattern($3); }

  | '+' K_WEIGHT NUMBER
    { if (defData->callbacks->SNetCbk) defData->Net->setWeight(ROUND($3)); }

  | '+' K_ESTCAP NUMBER
    { 
        // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
        if (defData->VersionNum < 5.5) {
            if (defData->callbacks->SNetCbk) {
                defData->Net->setCap($3);
            }
        }
    }

  | '+' K_USE use_type
    { if (defData->callbacks->SNetCbk) defData->Net->setUse($3); }

  | '+' K_STYLE NUMBER
    { 
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("SPECIALNETS ... [STYLE styleNum]")) {
                CHKERR();
            }
        }

        if (defData->callbacks->SNetCbk) {
            defData->Net->setStyle((int)$3); 
        }
    }

  | extension_stmt
    { CALLBACK(defData->callbacks->NetExtCbk, defrNetExtCbkType, &defData->History_text[0]); }

snet_type: K_FIXED
        { 
            defData->shieldName = NULL;
            defData->routeStatus = (char*)"FIXED"; 
            defData->dumb_mode = 1; 
        }
        | K_COVER 
        { 
            defData->shieldName = NULL;
            defData->routeStatus = (char*)"COVER"; 
            defData->dumb_mode = 1; 
        }
        | K_ROUTED 
        { 
            defData->shieldName = NULL;
            defData->routeStatus = (char*)"ROUTED"; 
            defData->dumb_mode = 1; 
        }
        | K_SHIELD  { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        {
            if (defData->VersionNum < 6.0 - 0.00001) {
                defData->routeStatus = (char*)"SHIELD";
            } 

            defData->dumb_mode = 1; 
            defData->no_num = 1; 

            defData->shieldName = $3;
        }
            
orient_pt: // empty 
        { $$ = 0; }
        | orient
        { $$ = $1; }
        

snet_width: '+' K_WIDTH { defData->dumb_mode = 1; } T_STRING NUMBER
            {
              // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
              if (defData->VersionNum < 5.5)
                 if (defData->callbacks->SNetCbk) defData->Net->setWidth($4, $5);
              else
                 defData->defWarning(7026, "The WIDTH statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
            }

snet_voltage: '+' K_VOLTAGE  { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
            {
              if (defrData::numIsInt($4)) {
                 if (defData->callbacks->SNetCbk) defData->Net->setVoltage(atoi($4));
              } else {
                 if (defData->callbacks->SNetCbk) {
                   if (defData->sNetWarnings++ < defData->settings->SNetWarnings) {
                     defData->defMsg = (char*)malloc(1000);
                     sprintf (defData->defMsg,
                        "The value %s for statement VOLTAGE is invalid. The value can only be integer.\nSpecify a valid value in units of millivolts", $4);
                     defData->defError(6537, defData->defMsg);
                     free(defData->defMsg);
                     CHKERR();
                   }
                 }
              }
            }

snet_spacing: '+' K_SPACING { defData->dumb_mode = 1; } T_STRING NUMBER
            {
              if (defData->callbacks->SNetCbk) defData->Net->setSpacing($4,$5);
            }
        opt_snet_range
            {
            }

opt_snet_range: // nothing 
        | K_RANGE NUMBER NUMBER
            {
              if (defData->callbacks->SNetCbk) defData->Net->setRange($2,$3);
            }

opt_range: // nothing 
        | K_RANGE NUMBER NUMBER
            { defData->Prop.setRange($2, $3); }


nets_pattern_type: K_BALANCED
            { 
                if (defData->VersionNum >= 6.0 - 0.00001) {
                    if (defData->def60ObsoletedError("NETS ... netName ... + PATTERN BALANCED")) {
                        CHKERR();
                    }
                }

                $$ = (char*)"BALANCED"; 
              }
        | K_STEINER
            { $$ = (char*)"STEINER"; }
        | K_TRUNK
            { $$ = (char*)"TRUNK"; }
        | K_WIREDLOGIC
            { 
                if (defData->VersionNum >= 6.0 - 0.00001) {
                    if (defData->def60ObsoletedError("NETS ... netName ... + PATTERN WIREDLOGIC")) {
                        CHKERR();
                    }
                }
                
                $$ = (char*)"WIREDLOGIC"; 
             }

snets_pattern_type:K_BALANCED
            { 
                if (defData->VersionNum >= 6.0 - 0.00001) {
                    if (defData->def60ObsoletedError("SPECIALNETS ... netName ... + PATTERN BALANCED")) {
                        CHKERR();
                    }
                }

                $$ = (char*)"BALANCED"; 
            }
        | K_STEINER
            { $$ = (char*)"STEINER"; }
        | K_TRUNK
            { $$ = (char*)"TRUNK"; }
        | K_WIREDLOGIC
            { 
                if (defData->VersionNum >= 6.0 - 0.00001) {
                    if (defData->def60ObsoletedError("SPECIALNETS ... netName ... + PATTERN WIREDLOGIC")) {
                        CHKERR();
                    }
                }
                
                $$ = (char*)"WIREDLOGIC"; 
             }

opt_swire:
  | {
        if (defData->callbacks->SNetCbk) {
            if ((defData->VersionNum < 5.4)
                && !(strcmp(defData->routeStatus, "SHIELD"))) { // PCR 445209
                defData->Shield = new defiShield(defData);
                defData->Shield->Init(defData->shieldName);
                defData->Net->addShield(defData->Shield, false);
            } else {
                if (defData->VersionNum >= 6.0 - 0.00001
                    && defData->shieldName) {
                    if (defData->def60ObsoletedError("SPECIALNETS ... + SHIELD specialWiring")) {
                        CHKERR();
                    }
                }

                defData->Wire = new defiWire(defData);
                defData->Wire->Init(defData->routeStatus,
                                    defData->shieldName);

                if (!defData->callbacks->WireInSNetCbk) {
                    defData->Net->addWire(defData->Wire);
                }
            }
        }
    }
    spaths
    {
        if (defData->callbacks->SNetCbk) {
            if (defData->callbacks->WireInSNetCbk
                && defData->Wire) {
                CALLBACK2(defData->callbacks->WireInSNetCbk,
                          defrWireInNetCbkType,
                          defData->Wire, defData->Net);

                delete defData->Wire;
            } else if (defData->callbacks->SNetWireCbk) {
                CALLBACK(defData->callbacks->SNetWireCbk,
                         defrSNetWireCbkType, defData->Net);
                if (defData->Shield)
                    defData->Net->freeShield();
                else
                    defData->Net->freeWire();
            }
        }

        defData->by_is_keyword = FALSE;
        defData->do_is_keyword = FALSE;
        defData->new_is_keyword = FALSE;
        defData->step_is_keyword = FALSE;
        defData->orient_is_keyword = FALSE;
        defData->virtual_is_keyword = FALSE;
        defData->mask_is_keyword = FALSE;
        defData->rect_is_keyword = FALSE;
        defData->width_is_keyword = FALSE;
        defData->needSNPCbk = 0; 
        defData->shieldName = NULL;
        defData->shapeType = NULL;
        defData->routeStatus = (char*)"ROUTED";
        defData->Shield = NULL;
        defData->Wire = NULL;
     }

spaths:
    {
        if (defData->callbacks->SNetCbk) {
            defData->PathObj = new defiPath(defData);
            defData->startPath();
        }
    }
    spath
    {
        if (defData->callbacks->SNetCbk) {
            if (defData->callbacks->PathInSNetWireCbk
                && defData->Wire) {
                CALLBACK3(defData->callbacks->PathInSNetWireCbk,
                          defrPathInWireCbkType,
                          defData->PathObj,
                          defData->Wire,
                          defData->Net);

                delete defData->PathObj;
            } else if (defData->needSNPCbk
                       && defData->callbacks->SNetPartialPathCbk) {
                // require a callback before proceed because defData->needSNPCbk
                // must be set to 1 from the previous finishPath and user has
                // registered a callback routine.
                CALLBACK(defData->callbacks->SNetPartialPathCbk,
                         defrSNetPartialPathCbkType,
                         defData->Net);
                defData->needSNPCbk = 0;   // reset the flag
                defData->finishPath(1, &defData->needSNPCbk);
                defData->Net->clearRectPolyNPath();
                defData->Net->clearVia();
            } else {
                defData->finishPath(0, &defData->needSNPCbk);
            }

            defData->PathObj = NULL;
        }
    }
  | spaths snew_path
    { }

snew_path: K_NEW
    {
        defData->dumb_mode = 1;

        if (defData->callbacks->SNetCbk) {
            defData->PathObj = new defiPath(defData);
            defData->startPath();
        }
    }
    spath
    {
        if (defData->callbacks->SNetCbk) {
            if (defData->callbacks->PathInSNetWireCbk
                && defData->Wire) {
                CALLBACK3(defData->callbacks->PathInSNetWireCbk,
                          defrPathInWireCbkType,
                          defData->PathObj,
                          defData->Wire,
                          defData->Net);

                delete defData->PathObj;
            } else if (defData->needSNPCbk
                       && defData->callbacks->SNetPartialPathCbk) {
                // require a callback before proceed because defData->needSNPCbk
                // must be set to 1 from the previous finishPath and user has
                // registered a callback routine.
                CALLBACK(defData->callbacks->SNetPartialPathCbk,
                         defrSNetPartialPathCbkType,
                         defData->Net);
                defData->needSNPCbk = 0;   // reset the flag
                defData->finishPath(1, &defData->needSNPCbk);
                // reset any poly or rect in special wiring statement
                defData->Net->clearRectPolyNPath();
                defData->Net->clearVia();
            } else {
                defData->finishPath(0, &defData->needSNPCbk);
            }
            
            defData->PathObj = NULL;
        }
    }

spath:
    T_STRING
    {
        if (defData->callbacks->SNetCbk) {
            defData->PathObj->addLayer($1);
        }
        
        defData->dumb_mode = 0;
        defData->no_num = 0;
    }
    width opt_spaths path_pt
    {
        defData->dumb_mode = DEF_MAX_INT;
        defData->by_is_keyword = TRUE;
        defData->do_is_keyword = TRUE;
        defData->new_is_keyword = TRUE;
        defData->step_is_keyword = TRUE;
        defData->orient_is_keyword = TRUE;
        defData->rect_is_keyword = TRUE;
        defData->mask_is_keyword = TRUE; 
        defData->virtual_is_keyword = TRUE;
    }
    path_item_list_opt
    {
        defData->dumb_mode = 0;
        defData->rect_is_keyword = FALSE;
        defData->mask_is_keyword = FALSE;
        defData->virtual_is_keyword = FALSE;
    }

width:
    NUMBER
    {
        if (defData->callbacks->SNetCbk) {
            defData->PathObj->addWidth(ROUND($1));
        }
    }

start_snets: K_SNETS NUMBER ';'
    { 
        defData->Net = new defiNet(defData);

        if (defData->callbacks->SNetStartCbk) {
            CALLBACK(defData->callbacks->SNetStartCbk,
                     defrSNetStartCbkType,
                     ROUND($2));
        }

        defData->netOsnet = 2;
    }

end_snets: K_END K_SNETS 
    { 
        if (defData->callbacks->SNetEndCbk) {
            CALLBACK(defData->callbacks->SNetEndCbk, defrSNetEndCbkType, 0);
        }

        defData->netOsnet = 0;

        delete defData->Net;
        defData->Net = NULL;
    }

groups_section: groups_start group_rules groups_end
      ;

groups_start: K_GROUPS NUMBER ';'
      {
        if (defData->callbacks->GroupsStartCbk)
           CALLBACK(defData->callbacks->GroupsStartCbk, defrGroupsStartCbkType, ROUND($2));
      }

group_rules: // empty 
      | group_rules group_rule
      ;

group_rule: start_group group_members group_options ';'
      {
        if (defData->callbacks->GroupCbk)
           CALLBACK(defData->callbacks->GroupCbk, defrGroupCbkType, &defData->Group);
      }

start_group: '-' { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING 
      {
        defData->dumb_mode = DEF_MAX_INT;
        defData->no_num = DEF_MAX_INT;
        /* dumb_mode is automatically turned off at the first
         * + in the options or at the ; at the end of the group */
        if (defData->callbacks->GroupCbk) defData->Group.setup($3);
        if (defData->callbacks->GroupNameCbk)
           CALLBACK(defData->callbacks->GroupNameCbk, defrGroupNameCbkType, $3);
      }

group_members: 
      | group_members group_member
      {  }

group_member: T_STRING
      {
        // if (defData->callbacks->GroupCbk) defData->Group.addMember($1); 
        if (defData->callbacks->GroupMemberCbk)
          CALLBACK(defData->callbacks->GroupMemberCbk, defrGroupMemberCbkType, $1);
      }

group_options: // empty 
      | group_options group_option 
      ;

group_option: '+' K_SOFT group_soft_options
      { }
      |     '+' K_PROPERTY { defData->dumb_mode = DEF_MAX_INT; }
            group_prop_list
      { defData->dumb_mode = 0; }
      |     '+' K_REGION { defData->dumb_mode = 1;  defData->no_num = 1; } group_region
      { }
      | extension_stmt
      { 
        if (defData->callbacks->GroupMemberCbk)
          CALLBACK(defData->callbacks->GroupExtCbk, defrGroupExtCbkType, &defData->History_text[0]);
      }
      | '+' K_POWERDOMAIN
      {
         if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("GROUPS ... - groupName ... + POWERDOMAIN")) {
                CHKERR();
            }
        } else { 
            if (defData->callbacks->GroupCbk) {
                defData->Group.setPowerdomain();
            }
          }      
      }

      | '+' K_HINSTS 
      {
         defData->dumb_mode = DEF_MAX_INT; 
         defData->no_num = DEF_MAX_INT;
      }
      group_hinsts
      { 
        defData->dumb_mode = 0; 
        defData->no_num = 0;

        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("GROUPS ... - groupName ... + HINSTS hinst1 ...")) {
                CHKERR();
            }
        } 
      }

      | '+' K_COMPS 
      {
         defData->dumb_mode = DEF_MAX_INT; 
         defData->no_num = DEF_MAX_INT;
      }      
      group_components
      { 
        defData->dumb_mode = 0; 
        defData->no_num = 0;
        
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("GROUPS ... - groupName ... + COMPONENTS component1 ...")) {
                CHKERR();
            }
        } 
      }

      | '+' K_GROUPS 
      {
         defData->dumb_mode = DEF_MAX_INT; 
         defData->no_num = DEF_MAX_INT;
      }
      group_groups
      { 
        defData->dumb_mode = 0; 
        defData->no_num = 0;

        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("GROUPS ... - groupName ... + GROUPS group1 ...")) {
                CHKERR();
            }
        } 
      }

group_hinsts: group_hinsts group_hinst 
      | group_hinst
      {}

group_hinst: T_STRING
      {
        if (defData->callbacks->GroupCbk) {
                defData->Group.addHinst($1);
        }
      }

group_components: group_components group_component 
      | group_component
      {}

group_component: T_STRING
      {
        if (defData->callbacks->GroupCbk) {
            defData->Group.addComponent($1);
        }
      }

group_groups: group_groups group_group 
      | group_group
      {}

group_group: T_STRING
      {
        if (defData->callbacks->GroupCbk) {
            defData->Group.addGroup($1);
        }
      }

group_region: pt pt
      {
        // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
        if (defData->VersionNum < 5.5) {
          if (defData->callbacks->GroupCbk)
            defData->Group.addRegionRect($1.x, $1.y, $2.x, $2.y);
        }
        else
          defData->defWarning(7027, "The GROUP REGION pt pt statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
      }
      | T_STRING
      { if (defData->callbacks->GroupCbk)
          defData->Group.setRegionName($1);
      }

group_prop_list : // empty 
      | group_prop_list group_prop
      ;

group_prop : T_STRING NUMBER
      {
        if (defData->callbacks->GroupCbk) {
          char propTp;
          char* str = defData->ringCopy("                       ");
          propTp = defData->session->GroupProp.propType($1);
          CHKPROPTYPE(propTp, $1, "GROUP");
          sprintf(str, "%g", $2);
          defData->Group.addNumProperty($1, $2, str, propTp);
        }
      }
      | T_STRING QSTRING
      {
        if (defData->callbacks->GroupCbk) {
          char propTp;
          propTp = defData->session->GroupProp.propType($1);
          CHKPROPTYPE(propTp, $1, "GROUP");
          defData->Group.addProperty($1, $2, propTp);
        }
      }
      | T_STRING T_STRING
      {
        if (defData->callbacks->GroupCbk) {
          char propTp;
          propTp = defData->session->GroupProp.propType($1);
          CHKPROPTYPE(propTp, $1, "GROUP");
          defData->Group.addProperty($1, $2, propTp);
        }
      }

group_soft_options: // empty 
      | group_soft_options group_soft_option 
      { }

group_soft_option: K_MAXX NUMBER
      {
        // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
        if (defData->VersionNum < 5.5)
          if (defData->callbacks->GroupCbk) defData->Group.setMaxX(ROUND($2));
        else
          defData->defWarning(7028, "The GROUP SOFT MAXX statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
      }
      | K_MAXY NUMBER
      { 
        // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
        if (defData->VersionNum < 5.5)
          if (defData->callbacks->GroupCbk) defData->Group.setMaxY(ROUND($2));
        else
          defData->defWarning(7029, "The GROUP SOFT MAXY statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
      }
      | K_MAXHALFPERIMETER NUMBER
      { 
        // 11/12/2002 - this is obsolete in 5.5, & will be ignored 
        if (defData->VersionNum < 5.5)
          if (defData->callbacks->GroupCbk) defData->Group.setPerim(ROUND($2));
        else
          defData->defWarning(7030, "The GROUP SOFT MAXHALFPERIMETER statement is obsolete in version 5.5 and later.\nThe DEF parser will ignore this statement.");
      }

groups_end: K_END K_GROUPS 
      { 
        if (defData->callbacks->GroupsEndCbk)
          CALLBACK(defData->callbacks->GroupsEndCbk, defrGroupsEndCbkType, 0);
      }

// 8/31/2001 - This is obsolete in 5.4 
assertions_section: assertions_start constraint_rules assertions_end
      ;

// 8/31/2001 - This is obsolete in 5.4 
constraint_section: constraints_start constraint_rules constraints_end
      ;

assertions_start: K_ASSERTIONS NUMBER ';'
      {
        if ((defData->VersionNum < 5.4) && (defData->callbacks->AssertionsStartCbk)) {
          CALLBACK(defData->callbacks->AssertionsStartCbk, defrAssertionsStartCbkType,
                   ROUND($2));
        } else {
          if (defData->callbacks->AssertionCbk)
            if (defData->assertionWarnings++ < defData->settings->AssertionWarnings)
              defData->defWarning(7031, "The ASSERTIONS statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
        }
        if (defData->callbacks->AssertionCbk)
          defData->Assertion.setAssertionMode();
      }

constraints_start: K_CONSTRAINTS NUMBER ';'
      {
        if ((defData->VersionNum < 5.4) && (defData->callbacks->ConstraintsStartCbk)) {
          CALLBACK(defData->callbacks->ConstraintsStartCbk, defrConstraintsStartCbkType,
                   ROUND($2));
        } else {
          if (defData->callbacks->ConstraintCbk)
            if (defData->constraintWarnings++ < defData->settings->ConstraintWarnings)
              defData->defWarning(7032, "The CONSTRAINTS statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
        }
        if (defData->callbacks->ConstraintCbk)
          defData->Assertion.setConstraintMode();
      }

constraint_rules: // empty 
      | constraint_rules constraint_rule 
      ;

constraint_rule:   operand_rule 
      | wiredlogic_rule 
      {
        if ((defData->VersionNum < 5.4) && (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)) {
          if (defData->Assertion.isConstraint()) 
            CALLBACK(defData->callbacks->ConstraintCbk, defrConstraintCbkType, &defData->Assertion);
          if (defData->Assertion.isAssertion()) 
            CALLBACK(defData->callbacks->AssertionCbk, defrAssertionCbkType, &defData->Assertion);
        }
      }

operand_rule: '-' operand delay_specs ';'
      { 
        if ((defData->VersionNum < 5.4) && (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)) {
          if (defData->Assertion.isConstraint()) 
            CALLBACK(defData->callbacks->ConstraintCbk, defrConstraintCbkType, &defData->Assertion);
          if (defData->Assertion.isAssertion()) 
            CALLBACK(defData->callbacks->AssertionCbk, defrAssertionCbkType, &defData->Assertion);
        }
   
        // reset all the flags and everything
        defData->Assertion.clear();
      }

operand: K_NET { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING 
      {
         if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
           defData->Assertion.addNet($3);
      }
      | K_PATH {defData->dumb_mode = 4; defData->no_num = 4;} T_STRING T_STRING T_STRING T_STRING
      {
         if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
           defData->Assertion.addPath($3, $4, $5, $6);
      }
      | K_SUM  '(' operand_list ')'
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
           defData->Assertion.setSum();
      }
      | K_DIFF '(' operand_list ')'
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
           defData->Assertion.setDiff();
      }

operand_list: operand 
      | operand_list operand
      { }
      | operand_list ',' operand

wiredlogic_rule: '-' K_WIREDLOGIC { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      opt_plus K_MAXDIST NUMBER ';'
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
          defData->Assertion.setWiredlogic($4, $7);
      }

opt_plus:
      // empty 
      { $$ = (char*)""; }
      | '+'
      { $$ = (char*)"+"; }

delay_specs: // empty 
      | delay_specs delay_spec
      ;

delay_spec: '+' K_RISEMIN NUMBER 
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
          defData->Assertion.setRiseMin($3);
      }
      | '+' K_RISEMAX NUMBER 
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
          defData->Assertion.setRiseMax($3);
      }
      | '+' K_FALLMIN NUMBER 
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
          defData->Assertion.setFallMin($3);
      }
      | '+' K_FALLMAX NUMBER 
      {
        if (defData->callbacks->ConstraintCbk || defData->callbacks->AssertionCbk)
          defData->Assertion.setFallMax($3);
      }

constraints_end: K_END K_CONSTRAINTS
      { if ((defData->VersionNum < 5.4) && defData->callbacks->ConstraintsEndCbk) {
          CALLBACK(defData->callbacks->ConstraintsEndCbk, defrConstraintsEndCbkType, 0);
        } else {
          if (defData->callbacks->ConstraintsEndCbk) {
            if (defData->constraintWarnings++ < defData->settings->ConstraintWarnings)
              defData->defWarning(7032, "The CONSTRAINTS statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
          }
        }
      }

assertions_end: K_END K_ASSERTIONS
      { if ((defData->VersionNum < 5.4) && defData->callbacks->AssertionsEndCbk) {
          CALLBACK(defData->callbacks->AssertionsEndCbk, defrAssertionsEndCbkType, 0);
        } else {
          if (defData->callbacks->AssertionsEndCbk) {
            if (defData->assertionWarnings++ < defData->settings->AssertionWarnings)
              defData->defWarning(7031, "The ASSERTIONS statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
          }
        }
      }

scanchains_section: scanchain_start scanchain_rules scanchain_end
      ;

scanchain_start: K_SCANCHAINS NUMBER ';'
      { if (defData->callbacks->ScanchainsStartCbk)
          CALLBACK(defData->callbacks->ScanchainsStartCbk, defrScanchainsStartCbkType,
                   ROUND($2));
      }

scanchain_rules: // empty 
      | scanchain_rules scan_rule
      {}

scan_rule: start_scan scan_members ';' 
      { 
        if (defData->callbacks->ScanchainCbk)
          CALLBACK(defData->callbacks->ScanchainCbk, defrScanchainCbkType, &defData->Scanchain);
      }

start_scan: '-' {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING 
      {
        if (defData->callbacks->ScanchainCbk) {
          defData->Scanchain.closeOrderedList();
          defData->Scanchain.setName($3);
        }
        defData->bit_is_keyword = TRUE;
      }

scan_members: 
      | scan_members scan_member
      ;

opt_pin :
      // empty 
      { $$ = (char*)""; }
      | T_STRING
      { $$ = $1; }

scan_member: '+' K_START {defData->dumb_mode = 2; defData->no_num = 2;} T_STRING opt_pin
      { 
        if (defData->callbacks->ScanchainCbk) {
          defData->Scanchain.closeOrderedList();
          defData->Scanchain.setStart($4, $5);
        }
      }
      | '+' K_FLOATING 
      {
         defData->dumb_mode = DEF_MAX_INT; 
         defData->no_num = DEF_MAX_INT;      
      } 
      floating_inst_list
      { 
        if (defData->callbacks->ScanchainCbk) {
           defData->Scanchain.closeOrderedList();     
        }

        defData->dumb_mode = 0; 
        defData->no_num = 0; 
      }
      | '+' K_ORDERED
      {
         if (defData->callbacks->ScanchainCbk) {
           defData->Scanchain.startOrderedList();
         }

         defData->dumb_mode = DEF_MAX_INT; 
         defData->no_num = DEF_MAX_INT;
      }
      ordered_inst_list_opt
      {         
         defData->dumb_mode = 0; 
         defData->no_num = 0; 
      }
      | '+' K_STOP {defData->dumb_mode = 2; defData->no_num = 2; } T_STRING opt_pin
      { 
        if (defData->callbacks->ScanchainCbk) {
          defData->Scanchain.setStop($4, $5);
          defData->Scanchain.closeOrderedList();
        }
      }
      | '+' K_COMMONSCANPINS { defData->dumb_mode = 10; defData->no_num = 10; } opt_common_pins
      { 
        if (defData->callbacks->ScanchainCbk) {
            defData->Scanchain.closeOrderedList();
        }
        
        defData->dumb_mode = 0;  
        defData->no_num = 0; 
      }
      | '+' K_PARTITION { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING  // 5.5 
      partition_maxbits
      {
        if (defData->VersionNum < 5.5) {
          if (defData->callbacks->ScanchainCbk) {
            if (defData->scanchainWarnings++ < defData->settings->ScanchainWarnings) {
              defData->defMsg = (char*)malloc(1000);
              sprintf (defData->defMsg,
                 "The PARTITION statement is available in version 5.5 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
              defData->defError(6538, defData->defMsg);
              free(defData->defMsg);
              CHKERR();
            }
          }
        }
        
        if (defData->callbacks->ScanchainCbk) {
          defData->Scanchain.setPartition($4, $5);
        }
      }
      | extension_stmt
      {
        if (defData->callbacks->ScanChainExtCbk) {
          CALLBACK(defData->callbacks->ScanChainExtCbk, defrScanChainExtCbkType, &defData->History_text[0]);
        }
      }
      | '+' K_PROPERTY 
      { 
        defData->dumb_mode = 2; 
      }       
      prop_name_value
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("SCANCHAINS ... + PROPERTY propName propValue")) {
                CHKERR();
            }
        } else if (defData->callbacks->ScanchainCbk) {
            defData->setPropDataType($4, "SCANCHAIN", defData->session->ScanChainProp);

            if (defData->Scanchain.hasOpenedOrderedList()) {
                defData->Scanchain.addOrderedProp($4);
            } else {
                defData->Scanchain.addProp($4);
            }
            
            $4 = NULL;
        }

        delete $4;
        defData->dumb_mode = DEF_MAX_INT; 
        defData->no_num = DEF_MAX_INT;
      }
      ordered_inst_list_opt
      {
         defData->dumb_mode = 0; 
         defData->no_num = 0;       
      }
      | '+' K_NAME { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("SCANCHAINS ... + ORDERED ... + NAME orderName")) {
                CHKERR();
            }
        } else if (defData->callbacks->ScanchainCbk) {
            if(!defData->Scanchain.hasOpenedOrderedList()) {
                defData->def60SyntaxError("SCANCHAINS ... + NAME orderName statement requires preceding + ORDERED statement");
            } else {
                defData->Scanchain.setOrderedName($4);
            }
        }

        defData->dumb_mode = DEF_MAX_INT; 
        defData->no_num = DEF_MAX_INT;
      }
      ordered_inst_list_opt
      {
         defData->dumb_mode = 0; 
         defData->no_num = 0;       
      }

opt_common_pins: // empty 
      { }
      | '(' T_STRING T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.setCommonIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.setCommonOut($3);
        }
      }
      | '(' T_STRING T_STRING ')' '(' T_STRING T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.setCommonIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.setCommonOut($3);
          if (strcmp($6, "IN") == 0 || strcmp($6, "in") == 0)
            defData->Scanchain.setCommonIn($7);
          else if (strcmp($6, "OUT") == 0 || strcmp($6, "out") == 0)
            defData->Scanchain.setCommonOut($7);
        }
      }

floating_inst_list: // empty 
      | floating_inst_list one_floating_inst
      ;

one_floating_inst: T_STRING
      {
        if (defData->callbacks->ScanchainCbk) {
          defData->Scanchain.addFloatingInst($1);
        }
      }
      floating_pins
      {}

floating_pins: // empty  
      {}
      | '(' T_STRING  T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addFloatingIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addFloatingOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
        }
      }
      | '(' T_STRING  T_STRING ')' '(' T_STRING  T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addFloatingIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addFloatingOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
          if (strcmp($6, "IN") == 0 || strcmp($6, "in") == 0)
            defData->Scanchain.addFloatingIn($7);
          else if (strcmp($6, "OUT") == 0 || strcmp($6, "out") == 0)
            defData->Scanchain.addFloatingOut($7);
          else if (strcmp($6, "BITS") == 0 || strcmp($6, "bits") == 0) {
            defData->bitsNum = atoi($7);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
        }
      }
      | '(' T_STRING  T_STRING ')' '(' T_STRING  T_STRING ')' '(' T_STRING
      T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addFloatingIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addFloatingOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
          if (strcmp($6, "IN") == 0 || strcmp($6, "in") == 0)
            defData->Scanchain.addFloatingIn($7);
          else if (strcmp($6, "OUT") == 0 || strcmp($6, "out") == 0)
            defData->Scanchain.addFloatingOut($7);
          else if (strcmp($6, "BITS") == 0 || strcmp($6, "bits") == 0) {
            defData->bitsNum = atoi($7);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
          if (strcmp($10, "IN") == 0 || strcmp($10, "in") == 0)
            defData->Scanchain.addFloatingIn($11);
          else if (strcmp($10, "OUT") == 0 || strcmp($10, "out") == 0)
            defData->Scanchain.addFloatingOut($11);
          else if (strcmp($10, "BITS") == 0 || strcmp($10, "bits") == 0) {
            defData->bitsNum = atoi($11);
            defData->Scanchain.setFloatingBits(defData->bitsNum);
          }
        }
      }
    
ordered_inst_list_opt: // empty 
      | ordered_inst_list 
      { 
      }

ordered_inst_list: ordered_inst_list one_ordered_inst
      | one_ordered_inst
      {}

one_ordered_inst: 
      { 
      }
      T_STRING
      { 
        if (defData->callbacks->ScanchainCbk) {
            if (!defData->Scanchain.hasOpenedOrderedList()) {
                defData->def60KeywordRequiresKeywordError("SCANCHAINS", 
                                                          "{fixedComp [ ( IN pin ) ] [ ( OUT pin ) ] [ ( BITS numBits ) ]",
                                                          "+ ORDERED");
            } else {
                defData->Scanchain.addOrderedInst($2);
            }
        }
      }
      ordered_pins
      { 
      }

ordered_pins: // empty  
      | '(' T_STRING  T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk && defData->Scanchain.hasOpenedOrderedList()) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addOrderedIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addOrderedOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
         }
        }
      }
      | '(' T_STRING  T_STRING ')' '(' T_STRING  T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk && defData->Scanchain.hasOpenedOrderedList()) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addOrderedIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addOrderedOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
          }
          if (strcmp($6, "IN") == 0 || strcmp($6, "in") == 0)
            defData->Scanchain.addOrderedIn($7);
          else if (strcmp($6, "OUT") == 0 || strcmp($6, "out") == 0)
            defData->Scanchain.addOrderedOut($7);
          else if (strcmp($6, "BITS") == 0 || strcmp($6, "bits") == 0) {
            defData->bitsNum = atoi($7);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
          }
        }
      }
      | '(' T_STRING  T_STRING ')' '(' T_STRING  T_STRING ')' '(' T_STRING
      T_STRING ')'
      {
        if (defData->callbacks->ScanchainCbk && defData->Scanchain.hasOpenedOrderedList()) {
          if (strcmp($2, "IN") == 0 || strcmp($2, "in") == 0)
            defData->Scanchain.addOrderedIn($3);
          else if (strcmp($2, "OUT") == 0 || strcmp($2, "out") == 0)
            defData->Scanchain.addOrderedOut($3);
          else if (strcmp($2, "BITS") == 0 || strcmp($2, "bits") == 0) {
            defData->bitsNum = atoi($3);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
          }
          if (strcmp($6, "IN") == 0 || strcmp($6, "in") == 0)
            defData->Scanchain.addOrderedIn($7);
          else if (strcmp($6, "OUT") == 0 || strcmp($6, "out") == 0)
            defData->Scanchain.addOrderedOut($7);
          else if (strcmp($6, "BITS") == 0 || strcmp($6, "bits") == 0) {
            defData->bitsNum = atoi($7);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
          }
          if (strcmp($10, "IN") == 0 || strcmp($10, "in") == 0)
            defData->Scanchain.addOrderedIn($11);
          else if (strcmp($10, "OUT") == 0 || strcmp($10, "out") == 0)
            defData->Scanchain.addOrderedOut($11);
          else if (strcmp($10, "BITS") == 0 || strcmp($10, "bits") == 0) {
            defData->bitsNum = atoi($11);
            defData->Scanchain.setOrderedBits(defData->bitsNum);
          }
        }
      }
    
partition_maxbits: // empty 
      { $$ = -1; }
      | K_MAXBITS NUMBER
      { $$ = ROUND($2); }
    
scanchain_end: K_END K_SCANCHAINS
      { 
        if (defData->callbacks->ScanchainsEndCbk)
          CALLBACK(defData->callbacks->ScanchainsEndCbk, defrScanchainsEndCbkType, 0);
        defData->bit_is_keyword = FALSE;
        defData->dumb_mode = 0; defData->no_num = 0;
      }

// 8/31/2001 - This is obsolete in 5.4 
iotiming_section: iotiming_start iotiming_rules iotiming_end
      ;

iotiming_start: K_IOTIMINGS NUMBER ';'
      {
        if (defData->VersionNum < 5.4 && defData->callbacks->IOTimingsStartCbk) {
          CALLBACK(defData->callbacks->IOTimingsStartCbk, defrIOTimingsStartCbkType, ROUND($2));
        } else {
          if (defData->callbacks->IOTimingsStartCbk)
            if (defData->iOTimingWarnings++ < defData->settings->IOTimingWarnings)
              defData->defWarning(7035, "The IOTIMINGS statement is obsolete in version 5.4 and later.\nThe DEF parser will ignore this statement.");
        }
      }

iotiming_rules: // empty 
      | iotiming_rules iotiming_rule
      { }

iotiming_rule: start_iotiming iotiming_members ';' 
      { 
        if (defData->VersionNum < 5.4 && defData->callbacks->IOTimingCbk)
          CALLBACK(defData->callbacks->IOTimingCbk, defrIOTimingCbkType, &defData->IOTiming);
      } 

start_iotiming: '-' '(' {defData->dumb_mode = 2; defData->no_num = 2; } T_STRING T_STRING ')'
      {
        if (defData->callbacks->IOTimingCbk)
          defData->IOTiming.setName($4, $5);
      }

iotiming_members: 
      | iotiming_members iotiming_member
      ;

iotiming_member:
      '+' risefall K_VARIABLE NUMBER NUMBER
      {
        if (defData->callbacks->IOTimingCbk) 
          defData->IOTiming.setVariable($2, $4, $5);
      }
      | '+' risefall K_SLEWRATE NUMBER NUMBER
      {
        if (defData->callbacks->IOTimingCbk) 
          defData->IOTiming.setSlewRate($2, $4, $5);
      }
      | '+' K_CAPACITANCE NUMBER
      {
        if (defData->callbacks->IOTimingCbk) 
          defData->IOTiming.setCapacitance($3);
      }
      | '+' K_DRIVECELL {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->IOTimingCbk) 
          defData->IOTiming.setDriveCell($4);
      } iotiming_drivecell_opt
      // | '+' K_FROMPIN   {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING

      // | '+' K_PARALLEL NUMBER

      | extension_stmt
      {
        if (defData->VersionNum < 5.4 && defData->callbacks->IoTimingsExtCbk)
          CALLBACK(defData->callbacks->IoTimingsExtCbk, defrIoTimingsExtCbkType, &defData->History_text[0]);
      }

iotiming_drivecell_opt: iotiming_frompin
      K_TOPIN {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->IOTimingCbk) 
          defData->IOTiming.setTo($4);
      }
      iotiming_parallel

iotiming_frompin: // empty 
      | K_FROMPIN {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->IOTimingCbk)
          defData->IOTiming.setFrom($3);
      }

iotiming_parallel: // empty 
      | K_PARALLEL NUMBER
      {
        if (defData->callbacks->IOTimingCbk)
          defData->IOTiming.setParallel($2);
      }

risefall: K_RISE { $$ = (char*)"RISE"; } | K_FALL { $$ = (char*)"FALL"; }

iotiming_end: K_END K_IOTIMINGS
      {
        if (defData->VersionNum < 5.4 && defData->callbacks->IOTimingsEndCbk)
          CALLBACK(defData->callbacks->IOTimingsEndCbk, defrIOTimingsEndCbkType, 0);
      }

floorplan_contraints_section: fp_start fp_stmts K_END K_FPC
      { 
        if (defData->callbacks->FPCEndCbk)
          CALLBACK(defData->callbacks->FPCEndCbk, defrFPCEndCbkType, 0);
      }

fp_start: K_FPC NUMBER ';'
      {
        if (defData->callbacks->FPCStartCbk)
          CALLBACK(defData->callbacks->FPCStartCbk, defrFPCStartCbkType, ROUND($2));
      }

fp_stmts: // empty 
      | fp_stmts fp_stmt
      {}

fp_stmt: '-' { defData->dumb_mode = 1; defData->no_num = 1;  } T_STRING h_or_v
      { if (defData->callbacks->FPCCbk) defData->FPC.setName($3, $4); }
      constraint_type constrain_what_list ';'
      { if (defData->callbacks->FPCCbk) CALLBACK(defData->callbacks->FPCCbk, defrFPCCbkType, &defData->FPC); }

h_or_v: K_HORIZONTAL 
      { $$ = (char*)"HORIZONTAL"; }
      | K_VERTICAL
      { $$ = (char*)"VERTICAL"; }

constraint_type: K_ALIGN
      { if (defData->callbacks->FPCCbk) defData->FPC.setAlign(); }
      | K_MAX NUMBER
      { if (defData->callbacks->FPCCbk) defData->FPC.setMax($2); }
      | K_MIN NUMBER
      { if (defData->callbacks->FPCCbk) defData->FPC.setMin($2); }
      | K_EQUAL NUMBER
      { if (defData->callbacks->FPCCbk) defData->FPC.setEqual($2); }

constrain_what_list: // empty 
      | constrain_what_list constrain_what
      ;

constrain_what: '+' K_BOTTOMLEFT
      { if (defData->callbacks->FPCCbk) defData->FPC.setDoingBottomLeft(); }
      row_or_comp_list 
      |       '+' K_TOPRIGHT
      { if (defData->callbacks->FPCCbk) defData->FPC.setDoingTopRight(); }
      row_or_comp_list 
      ;

row_or_comp_list: // empty 
      | row_or_comp_list row_or_comp

row_or_comp: '(' K_ROWS  {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ')'
      { if (defData->callbacks->FPCCbk) defData->FPC.addRow($4); }
      |    '(' K_COMPS {defData->dumb_mode = 1; defData->no_num = 1; } T_STRING ')'
      { if (defData->callbacks->FPCCbk) defData->FPC.addComps($4); }

timingdisables_section: timingdisables_start timingdisables_rules
      timingdisables_end
      ;

timingdisables_start: K_TIMINGDISABLES NUMBER ';'
      { 
        if (defData->callbacks->TimingDisablesStartCbk)
          CALLBACK(defData->callbacks->TimingDisablesStartCbk, defrTimingDisablesStartCbkType,
                   ROUND($2));
      }

timingdisables_rules: // empty 
      | timingdisables_rules timingdisables_rule
      {}

timingdisables_rule: '-' K_FROMPIN { defData->dumb_mode = 2; defData->no_num = 2;  } T_STRING
      T_STRING K_TOPIN { defData->dumb_mode = 2; defData->no_num = 2;  } T_STRING T_STRING ';'
      {
        if (defData->callbacks->TimingDisableCbk) {
          defData->TimingDisable.setFromTo($4, $5, $8, $9);
          CALLBACK(defData->callbacks->TimingDisableCbk, defrTimingDisableCbkType,
                &defData->TimingDisable);
        }
      }
      | '-' K_THRUPIN {defData->dumb_mode = 2; defData->no_num = 2; } T_STRING T_STRING ';'
      {
        if (defData->callbacks->TimingDisableCbk) {
          defData->TimingDisable.setThru($4, $5);
          CALLBACK(defData->callbacks->TimingDisableCbk, defrTimingDisableCbkType,
                   &defData->TimingDisable);
        }
      }
      | '-' K_MACRO {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING td_macro_option ';'
      {
        if (defData->callbacks->TimingDisableCbk) {
          defData->TimingDisable.setMacro($4);
          CALLBACK(defData->callbacks->TimingDisableCbk, defrTimingDisableCbkType,
                &defData->TimingDisable);
        }
      }
      | '-' K_REENTRANTPATHS ';'
      { if (defData->callbacks->TimingDisableCbk)
          defData->TimingDisable.setReentrantPathsFlag();
      }


td_macro_option: K_FROMPIN {defData->dumb_mode = 1; defData->no_num = 1;} T_STRING K_TOPIN
      {defData->dumb_mode=1; defData->no_num = 1;} T_STRING
      {
        if (defData->callbacks->TimingDisableCbk)
          defData->TimingDisable.setMacroFromTo($3,$6);
      }
      |        K_THRUPIN {defData->dumb_mode=1; defData->no_num = 1;} T_STRING
      {
        if (defData->callbacks->TimingDisableCbk)
          defData->TimingDisable.setMacroThru($3);
      }

timingdisables_end: K_END K_TIMINGDISABLES
      { 
        if (defData->callbacks->TimingDisablesEndCbk)
          CALLBACK(defData->callbacks->TimingDisablesEndCbk, defrTimingDisablesEndCbkType, 0);
      }


partitions_section: partitions_start partition_rules partitions_end
      ;

partitions_start: K_PARTITIONS NUMBER ';'
      {
        if (defData->callbacks->PartitionsStartCbk)
          CALLBACK(defData->callbacks->PartitionsStartCbk, defrPartitionsStartCbkType,
                   ROUND($2));
      }

partition_rules: // empty 
      | partition_rules partition_rule
      { }

partition_rule: start_partition partition_members ';' 
      { 
        if (defData->callbacks->PartitionCbk)
          CALLBACK(defData->callbacks->PartitionCbk, defrPartitionCbkType, &defData->Partition);
      }

start_partition: '-' { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING turnoff
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setName($3);
      }

turnoff: // empty 
      | K_TURNOFF turnoff_setup turnoff_hold
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.addTurnOff($2, $3);
      }

turnoff_setup: // empty 
      { $$ = (char*)" "; }
      | K_SETUPRISE
      { $$ = (char*)"R"; }
      | K_SETUPFALL
      { $$ = (char*)"F"; }

turnoff_hold: // empty 
      { $$ = (char*)" "; }
      | K_HOLDRISE
      { $$ = (char*)"R"; }
      | K_HOLDFALL
      { $$ = (char*)"F"; }

partition_members: // empty 
      | partition_members partition_member
      ;

partition_member: '+' K_FROMCLOCKPIN {defData->dumb_mode=2; defData->no_num = 2;}
      T_STRING T_STRING risefall minmaxpins
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setFromClockPin($4, $5);
      }
      | '+' K_FROMCOMPPIN {defData->dumb_mode=2; defData->no_num = 2; }
      T_STRING T_STRING risefallminmax2_list
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setFromCompPin($4, $5);
      }
      | '+' K_FROMIOPIN {defData->dumb_mode=1; defData->no_num = 1; } T_STRING
      risefallminmax1_list
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setFromIOPin($4);
      }
      | '+' K_TOCLOCKPIN {defData->dumb_mode=2; defData->no_num = 2; }
      T_STRING T_STRING risefall minmaxpins
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setToClockPin($4, $5);
      }
      | '+' K_TOCOMPPIN {defData->dumb_mode=2; defData->no_num = 2; }
      T_STRING T_STRING risefallminmax2_list
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setToCompPin($4, $5);
      }
      | '+' K_TOIOPIN {defData->dumb_mode=1; defData->no_num = 2; } T_STRING risefallminmax1_list
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setToIOPin($4);
      }
      | extension_stmt
      { 
        if (defData->callbacks->PartitionsExtCbk)
          CALLBACK(defData->callbacks->PartitionsExtCbk, defrPartitionsExtCbkType,
                   &defData->History_text[0]);
      }

minmaxpins: min_or_max_list K_PINS
      { defData->dumb_mode = DEF_MAX_INT; defData->no_num = DEF_MAX_INT; } pin_list
      { defData->dumb_mode = 0; defData->no_num = 0; }

min_or_max_list: // empty 
      | min_or_max_list min_or_max_member
      { }

min_or_max_member: K_MIN NUMBER NUMBER
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setMin($2, $3);
      }
      | K_MAX NUMBER NUMBER
      {
        if (defData->callbacks->PartitionCbk)
          defData->Partition.setMax($2, $3);
      }

pin_list: // empty 
      | pin_list T_STRING
      { if (defData->callbacks->PartitionCbk) defData->Partition.addPin($2); }

risefallminmax1_list: // empty 
      | risefallminmax1_list risefallminmax1

risefallminmax1: K_RISEMIN NUMBER
      { if (defData->callbacks->PartitionCbk) defData->Partition.addRiseMin($2); }
      | K_FALLMIN NUMBER
      { if (defData->callbacks->PartitionCbk) defData->Partition.addFallMin($2); }
      | K_RISEMAX NUMBER
      { if (defData->callbacks->PartitionCbk) defData->Partition.addRiseMax($2); }
      | K_FALLMAX NUMBER
      { if (defData->callbacks->PartitionCbk) defData->Partition.addFallMax($2); }

risefallminmax2_list:
      risefallminmax2
      | risefallminmax2_list risefallminmax2
      ;

risefallminmax2: K_RISEMIN NUMBER NUMBER
      { if (defData->callbacks->PartitionCbk)
          defData->Partition.addRiseMinRange($2, $3); }
      | K_FALLMIN NUMBER NUMBER
      { if (defData->callbacks->PartitionCbk)
          defData->Partition.addFallMinRange($2, $3); }
      | K_RISEMAX NUMBER NUMBER
      { if (defData->callbacks->PartitionCbk)
          defData->Partition.addRiseMaxRange($2, $3); }
      | K_FALLMAX NUMBER NUMBER
      { if (defData->callbacks->PartitionCbk)
          defData->Partition.addFallMaxRange($2, $3); }

partitions_end: K_END K_PARTITIONS
      { if (defData->callbacks->PartitionsEndCbk)
          CALLBACK(defData->callbacks->PartitionsEndCbk, defrPartitionsEndCbkType, 0); }

comp_names: // empty 
      | comp_names comp_name
      { }

comp_name: '(' {defData->dumb_mode=2; defData->no_num = 2; } T_STRING
      T_STRING subnet_opt_syn ')'
      {
        // note that the defData->first T_STRING could be the keyword VPIN 
        if (defData->callbacks->NetCbk)
          defData->Subnet->addPin($3, $4, $5);
      }

subnet_opt_syn: // empty 
      { $$ = 0; }
      | '+' K_SYNTHESIZED
      { 
        $$ = 1; 
      }

subnet_options: // empty 
      | subnet_options subnet_option

subnet_option: subnet_type
      {
        if (defData->callbacks->NetCbk) {
            defData->Wire = new defiWire(defData);
            defData->Wire->Init($1, NULL);
            defData->Subnet->addWire(defData->Wire);
        }
      }
      paths
      {  
        defData->by_is_keyword = FALSE;
        defData->do_is_keyword = FALSE;
        defData->new_is_keyword = FALSE;
        defData->step_is_keyword = FALSE;
        defData->orient_is_keyword = FALSE;
        defData->needNPCbk = 0;
        defData->Wire = NULL;
      }
      | K_NONDEFAULTRULE { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      { if (defData->callbacks->NetCbk) defData->Subnet->setNonDefault($3); }

subnet_type: K_FIXED
      { $$ = (char*)"FIXED"; defData->dumb_mode = 1; }
      | K_COVER
      { $$ = (char*)"COVER"; defData->dumb_mode = 1; }
      | K_ROUTED
      { $$ = (char*)"ROUTED"; defData->dumb_mode = 1; }
      | K_NOSHIELD
      { $$ = (char*)"NOSHIELD"; defData->dumb_mode = 1; }

pin_props_section: begin_pin_props pin_prop_list end_pin_props ;

begin_pin_props: K_PINPROPERTIES NUMBER opt_semi
      { 
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("PINPROPERTIES num ; ... END PINPROPERTIES")) {
                CHKERR();
            }
        }

        if (defData->callbacks->PinPropStartCbk) {
            CALLBACK(defData->callbacks->PinPropStartCbk, defrPinPropStartCbkType, ROUND($2)); 
        }
      }

opt_semi:
      // empty 
      { }
      | ';'
      { }

end_pin_props: K_END K_PINPROPERTIES
      { if (defData->callbacks->PinPropEndCbk)
          CALLBACK(defData->callbacks->PinPropEndCbk, defrPinPropEndCbkType, 0); }

pin_prop_list: // empty 
      | pin_prop_list pin_prop_terminal
      ;

pin_prop_terminal: '-' { defData->dumb_mode = 2; defData->no_num = 2; } T_STRING T_STRING
      { if (defData->callbacks->PinPropCbk) defData->PinProp.setName($3, $4); }
      pin_prop_options ';'
      { if (defData->callbacks->PinPropCbk) {
          CALLBACK(defData->callbacks->PinPropCbk, defrPinPropCbkType, &defData->PinProp);
         // reset the property number
         defData->PinProp.clear();
        }
      }

pin_prop_options : // empty 
      | pin_prop_options pin_prop ;

pin_prop: '+' K_PROPERTY { defData->dumb_mode = DEF_MAX_INT; }
      pin_prop_name_value_list 
      { defData->dumb_mode = 0; }

pin_prop_name_value_list : // empty 
      | pin_prop_name_value_list pin_prop_name_value
      ;

pin_prop_name_value : T_STRING NUMBER
      {
        if (defData->callbacks->PinPropCbk) {
          char propTp;
          char* str = defData->ringCopy("                       ");
          propTp = defData->session->CompPinProp.propType($1);
          CHKPROPTYPE(propTp, $1, "PINPROPERTIES");
          sprintf(str, "%g", $2);
          defData->PinProp.addNumProperty($1, $2, str, propTp);
        }
      }
 | T_STRING QSTRING
      {
        if (defData->callbacks->PinPropCbk) {
          char propTp;
          propTp = defData->session->CompPinProp.propType($1);
          CHKPROPTYPE(propTp, $1, "PINPROPERTIES");
          defData->PinProp.addProperty($1, $2, propTp);
        }
      }
 | T_STRING T_STRING
      {
        if (defData->callbacks->PinPropCbk) {
          char propTp;
          propTp = defData->session->CompPinProp.propType($1);
          CHKPROPTYPE(propTp, $1, "PINPROPERTIES");
          defData->PinProp.addProperty($1, $2, propTp);
        }
      }

blockage_section: blockage_start blockage_defs blockage_end ;

blockage_start: K_BLOCKAGES NUMBER ';'
      { if (defData->callbacks->BlockageStartCbk)
          CALLBACK(defData->callbacks->BlockageStartCbk, defrBlockageStartCbkType, ROUND($2)); }

blockage_end: K_END K_BLOCKAGES
      { if (defData->callbacks->BlockageEndCbk)
          CALLBACK(defData->callbacks->BlockageEndCbk, defrBlockageEndCbkType, 0); }

blockage_defs: // empty 
      | blockage_defs blockage_def
      ;

blockage_def: blockage_rule rectPoly_blockage rectPoly_blockage_rules
      ';'
      {
        if (defData->callbacks->BlockageCbk) {
          CALLBACK(defData->callbacks->BlockageCbk, defrBlockageCbkType, &defData->Blockage);
          defData->Blockage.clear();
        }
      }

blockage_rule: '-' K_LAYER { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING 
      {
        if (defData->callbacks->BlockageCbk) {
          if (defData->Blockage.hasPlacement() != 0) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6539, "Invalid BLOCKAGE statement defined in the DEF file. The BLOCKAGE statment has both the LAYER and the PLACEMENT statements defined.\nUpdate your DEF file to have either BLOCKAGE or PLACEMENT statement only.");
              CHKERR();
            }
          }
          defData->Blockage.setLayer($4);
          defData->Blockage.clearPoly(); // free poly, if any
        }
        defData->hasBlkLayerComp = 0;
        defData->hasBlkLayerSpac = 0;
        defData->hasBlkLayerTypeComp = 0;
      }

      layer_blockage_rules
      // 10/29/2001 - enhancement 
      | '-' K_PLACEMENT
      {
        if (defData->callbacks->BlockageCbk) {
          if (defData->Blockage.hasLayer() != 0) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6539, "Invalid BLOCKAGE statement defined in the DEF file. The BLOCKAGE statment has both the LAYER and the PLACEMENT statements defined.\nUpdate your DEF file to have either BLOCKAGE or PLACEMENT statement only.");
              CHKERR();
            }
          }
          defData->Blockage.setPlacement();
          defData->Blockage.clearPoly(); // free poly, if any
        }
        defData->hasBlkPlaceComp = 0;
        defData->hasBlkPlaceTypeComp = 0;
        defData->hasDef60BlkPlaceTypeComp = 0;
      }
      placement_comp_rules
      
layer_blockage_rules: // empty 
      | layer_blockage_rules layer_blockage_rule
      ;

layer_blockage_rule: '+' K_SPACING NUMBER
      {
        if (defData->VersionNum < 5.6) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defMsg = (char*)malloc(1000);
              sprintf (defData->defMsg,
                 "The SPACING statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
              defData->defError(6540, defData->defMsg);
              free(defData->defMsg);
              CHKERR();
            }
          }
        } else if (defData->hasBlkLayerSpac) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6541, "The SPACING statement is defined in the LAYER statement,\nbut there is already either a SPACING statement or DESIGNRULEWIDTH  statement has defined in the LAYER statement.\nUpdate your DEF file to have either SPACING statement or a DESIGNRULEWIDTH statement.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk)
            defData->Blockage.setSpacing((int)$3);
          defData->hasBlkLayerSpac = 1;
        }
      }
      | '+' K_DESIGNRULEWIDTH NUMBER
      {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("BLOCKAGES ... - LAYER ... DESIGNRULEWIDTH effectiveWidth")) {
                CHKERR();
            }
        } else if (defData->VersionNum < 5.6) {
            if (defData->callbacks->BlockageCbk) {
                if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                    defData->defError(6541, "The SPACING statement is defined in the LAYER statement,\nbut there is already either a SPACING statement or DESIGNRULEWIDTH  statement has defined in the LAYER statement.\nUpdate your DEF file to have either SPACING statement or a DESIGNRULEWIDTH statement.");
                    CHKERR();
                }
            }
        } else if (defData->hasBlkLayerSpac) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6541, "The SPACING statement is defined in the LAYER statement,\nbut there is already either a SPACING statement or DESIGNRULEWIDTH  statement has defined in the LAYER statement.\nUpdate your DEF file to have either SPACING statement or a DESIGNRULEWIDTH statement.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk)
            defData->Blockage.setDesignRuleWidth((int)$3);
          defData->hasBlkLayerSpac = 1;
        }
      }
      
      | '+' K_MASK NUMBER
      {      
        if (defData->validateMaskInput((int)$3, defData->blockageWarnings, defData->settings->BlockageWarnings)) {
          defData->Blockage.setMask((int)$3);
        }
      } 

      | '+' K_NAME { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      { 
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - LAYER layerName ... + NAME name")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->BlockageCbk) {
                defData->Blockage.setName($4);
            }
        }
      } 
     | '+' K_PARTIAL NUMBER         // 6.0
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - LAYER layerName ... + PARTIAL density")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->BlockageCbk) {
                defData->Blockage.setPartial($3);
            }
        }
      }
      | '+' K_PROPERTY 
      { 
        defData->dumb_mode = 2; 
      }       
      prop_name_value
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - LAYER layerName ... + PROPERTY propName propVal")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->BlockageCbk) {
                defData->setPropDataType($4, "PIN", defData->session->BlockageProp);
                defData->Blockage.addProp($4);
                $4 = 0;
            }
        }

        delete $4;
      }
      | comp_blockage_rule
      ;

comp_blockage_rule:
      // 06/20/2001 - pcr 383204 = pcr 378102 
      '+' K_COMPONENT { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->hasBlkLayerComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk) {
            defData->Blockage.setComponent($4);
          }
          if (defData->VersionNum < 5.8) {
            defData->hasBlkLayerComp = 1;
          }
        }
      }
      // 8/30/2001 - pcr 394394 
      | '+' K_SLOTS
      {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("BLOCKAGES ... - LAYER ... + COMPONENT compName + SLOTS")) {
                CHKERR();
            }
        } else if (defData->hasBlkLayerComp || defData->hasBlkLayerTypeComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk) {
            defData->Blockage.setSlots();
          }
          if (defData->VersionNum < 5.8) {
            defData->hasBlkLayerComp = 1;
          } else {
            defData->hasBlkLayerTypeComp = 1;
          }
        }
      }
      | '+' K_FILLS
      {
        if (defData->hasBlkLayerComp || defData->hasBlkLayerTypeComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk){
            defData->Blockage.setFills();
          }

          if (defData->VersionNum < 5.8) {
            defData->hasBlkLayerComp = 1;
          } else {
            defData->hasBlkLayerTypeComp = 1;
          }
        }
      }
      | '+' K_PUSHDOWN
      {
        if (defData->hasBlkLayerComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk){
            defData->Blockage.setPushdown();
          }
          if (defData->VersionNum < 5.8) {
            defData->hasBlkLayerComp = 1;
          }
        }
      }
      | '+' K_EXCEPTPGNET              // 5.7 
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->BlockageCbk) {
             if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
               defData->defMsg = (char*)malloc(10000);
               sprintf (defData->defMsg,
                 "The EXCEPTPGNET is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
               defData->defError(6549, defData->defMsg);
               free(defData->defMsg);
               CHKERR();
              }
           }
        } else {
           if (defData->hasBlkLayerComp) {
             if (defData->callbacks->BlockageCbk) {
               if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                 defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
                 CHKERR();
               }
             }
           } else {
             if (defData->callbacks->BlockageCbk){
               defData->Blockage.setExceptpgnet();
             }
             if (defData->VersionNum < 5.8){
               defData->hasBlkLayerComp = 1;
             }
           }
        }
      }
      | '+' K_ONLYPGNET              // 6.0 
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - LAYER layerName ... + ONLYPGNET")) {
                CHKERR();
            }
        } else {
           if (defData->hasBlkLayerComp) {
             if (defData->callbacks->BlockageCbk) {
               if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                 defData->defError(6542, "The defined BLOCKAGES COMPONENT statement has either COMPONENT, SLOTS, FILLS, PUSHDOWN, ONLYPGNET or EXCEPTPGNET defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES COMPONENT statement per layer.");
                 CHKERR();
               }
             }
           } else {
             if (defData->callbacks->BlockageCbk) {
               defData->Blockage.setOnlypgnet();
             }
             
             if (defData->VersionNum < 5.8) {
               defData->hasBlkLayerComp = 1;
             }
           }
        }
      }

placement_comp_rules: // empty 
      | placement_comp_rules placement_comp_rule
      ;
      
placement_comp_rule: // empty 
      // 10/29/2001 - enhancement 
      '+' K_COMPONENT { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->hasBlkPlaceComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6543, "The defined BLOCKAGES PLACEMENT statement has either COMPONENT, PUSHDOWN, SOFT or PARTIAL defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES PLACEMENT statement.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk){
            defData->Blockage.setComponent($4);
          }
          if (defData->VersionNum < 5.8) {
            defData->hasBlkPlaceComp = 1;
          }
        }
      }
      | '+' K_NAME { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      { 
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - PLACEMENT ... + NAME name")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->BlockageCbk) {
                defData->Blockage.setName($4);
            }
        }
      } 
      | '+' K_PROPERTY 
      { 
        defData->dumb_mode = 2; 
      }       
      prop_name_value
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - PLACEMENT ... + PROPERTY propName propVal")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->BlockageCbk) {
                defData->setPropDataType($4, "PIN", defData->session->BlockageProp);
                defData->Blockage.addProp($4);
                $4 = 0;
            }
        }

        delete $4;
      }      
      | '+' K_ONLYBLOCKS
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - PLACEMENT ... + ONLYBLOCKS")) {
                CHKERR();
            }
        } else if (defData->hasDef60BlkPlaceTypeComp) {
             if (defData->def60ExclusiveStatementsError("BLOCKAGES ... - PLACEMENT", "+ ONLYBLOCKS, + SOFT, + PARTIAL")) {
                CHKERR();
             }
        } else {
            if (defData->callbacks->BlockageCbk){
                defData->Blockage.setOnlyblocks();
            }

            defData->hasDef60BlkPlaceTypeComp = 1;
        }
      }
      | '+' K_PUSHDOWN
      {
        if (defData->hasBlkPlaceComp) {
          if (defData->callbacks->BlockageCbk) {
            if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
              defData->defError(6543, "The defined BLOCKAGES PLACEMENT statement has either COMPONENT, PUSHDOWN, SOFT or PARTIAL defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES PLACEMENT statement.");
              CHKERR();
            }
          }
        } else {
          if (defData->callbacks->BlockageCbk){
            defData->Blockage.setPushdown();
          }
          if (defData->VersionNum < 5.8) {
            defData->hasBlkPlaceComp = 1;
          }
        }
      }
      | '+' K_SOFT                   // 5.7
      {
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("BLOCKAGES ... - PLACEMENT ... + SOFT ...")) {
                CHKERR();
            }
        } else if (defData->VersionNum < 5.7) {
           if (defData->callbacks->BlockageCbk) {
             if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
               defData->defMsg = (char*)malloc(10000);
               sprintf (defData->defMsg,
                 "The PLACEMENT SOFT is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
               defData->defError(6547, defData->defMsg);
               free(defData->defMsg);
               CHKERR();
             }
           }
        } else {
           if (defData->hasBlkPlaceComp || defData->hasBlkPlaceTypeComp) {
             if (defData->callbacks->BlockageCbk) {
               if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                 defData->defError(6543, "The defined BLOCKAGES PLACEMENT statement has either COMPONENT, PUSHDOWN, SOFT or PARTIAL defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES PLACEMENT statement.");
                 CHKERR();
               }
             }
           } else {
             if (defData->callbacks->BlockageCbk){
               defData->Blockage.setSoft();
             }
             if (defData->VersionNum < 5.8) {
               defData->hasBlkPlaceComp = 1;
             }
             if (defData->VersionNum == 5.8) {
               defData->hasBlkPlaceTypeComp = 1;
             }
           }
        }
      }
      | '+' K_SOFT NUMBER            // 6.0
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - PLACEMENT ... + SOFT maxDensity")) {
                CHKERR();
            }
        } else if (defData->hasDef60BlkPlaceTypeComp) {
             if (defData->def60ExclusiveStatementsError("BLOCKAGES ... - PLACEMENT", "+ ONLYBLOCKS, + SOFT, + PARTIAL")) {
                CHKERR();
             }
        } else {
            if (defData->callbacks->BlockageCbk){
                defData->Blockage.setSoft($3);
            }

            defData->hasDef60BlkPlaceTypeComp = 1;
        }      
      }

      | '+' K_PARTIAL NUMBER        // 5.7
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->BlockageCbk) {
             if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                defData->defMsg = (char*)malloc(10000);
                sprintf (defData->defMsg,
                  "The PARTIAL is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
                defData->defError(6548, defData->defMsg);
                free(defData->defMsg);
                CHKERR();
             }
           }
        } else {
           if (defData->hasBlkPlaceComp || defData->hasBlkPlaceTypeComp) {
             if (defData->callbacks->BlockageCbk) {
               if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                 defData->defError(6543, "The defined BLOCKAGES PLACEMENT statement has either COMPONENT, PUSHDOWN, SOFT or PARTIAL defined.\nOnly one of these statements is allowed per LAYER. Updated the DEF file to define a valid BLOCKAGES PLACEMENT statement.");
                 CHKERR();
               }
             }
           } else if (defData->VersionNum >= 6.0 - 0.00001 && defData->hasDef60BlkPlaceTypeComp) {
             if (defData->def60ExclusiveStatementsError("BLOCKAGES ... - PLACEMENT", "+ ONLYBLOCKS, + SOFT, + PARTIAL")) {
                CHKERR();
             } 
           } else {
             if (defData->callbacks->BlockageCbk){
               defData->Blockage.setPartial($3);
             } 

             if (defData->VersionNum < 5.8) {
               defData->hasBlkPlaceComp = 1;
             } else if (defData->VersionNum == 5.8) {
               defData->hasBlkPlaceTypeComp = 1;
             } else if (defData->VersionNum >= 6.0 - 0.00001) {
                defData->hasDef60BlkPlaceTypeComp = 1;
             }
           }
         }
      }
           
      | '+' K_PARTIAL NUMBER K_NOFLOPS       // 6.0
      {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("BLOCKAGES ... - PLACEMENT ... + K_PARTIAL maxDensity K_NOFLOPS")) {
                CHKERR();
            }
        } else if (defData->hasDef60BlkPlaceTypeComp) {
            if (defData->def60ExclusiveStatementsError("BLOCKAGES ... - PLACEMENT", "+ ONLYBLOCKS, + SOFT, + PARTIAL")) {
                CHKERR();
            }
        } else if (defData->callbacks->BlockageCbk){
                defData->Blockage.setPartial($3, 1);
        }

        defData->hasDef60BlkPlaceTypeComp = 1;
      }

rectPoly_blockage_rules: // empty 
      | rectPoly_blockage_rules rectPoly_blockage
      ;
  
rectPoly_blockage: K_RECT pt pt
      {
        if (defData->callbacks->BlockageCbk)
          defData->Blockage.addRect($2.x, $2.y, $3.x, $3.y);
      }
      | K_POLYGON
      {
        if (defData->callbacks->BlockageCbk) {
            defData->Geometries.Reset();
        }
      }
      firstPt nextPt nextPt otherPts
      {
        if (defData->callbacks->BlockageCbk) {
          if (defData->VersionNum >= 5.6) {  // only 5.6 and beyond
            if (defData->Blockage.hasLayer()) {  // only in layer
              if (defData->callbacks->BlockageCbk)
                defData->Blockage.addPolygon(&defData->Geometries);
            } else {
              if (defData->callbacks->BlockageCbk) {
                if (defData->blockageWarnings++ < defData->settings->BlockageWarnings) {
                  defData->defError(6544, "A POLYGON statement is defined in the BLOCKAGE statement,\nbut it is not defined in the BLOCKAGE LAYER statement.\nUpdate your DEF file to either remove the POLYGON statement from the BLOCKAGE statement or\ndefine the POLYGON statement in a BLOCKAGE LAYER statement.");
                  CHKERR();
                }
              }
            }
          }
        }
      }

// 8/31/2001 - 5.4 enhancement 
slot_section: slot_start slot_defs slot_end ;

slot_start: K_SLOTS NUMBER ';'
      { 
        if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("SLOTS ... END SLOTS")) {
                CHKERR();
            }
        } else if (defData->callbacks->SlotStartCbk) {
            CALLBACK(defData->callbacks->SlotStartCbk, defrSlotStartCbkType, ROUND($2)); 
        }
      }

slot_end: K_END K_SLOTS
      { if (defData->callbacks->SlotEndCbk)
          CALLBACK(defData->callbacks->SlotEndCbk, defrSlotEndCbkType, 0); 
      }

slot_defs: // empty 
      | slot_defs slot_def
      ;

slot_def: slot_rule geom_slot_rules ';'
      {
        if (defData->callbacks->SlotCbk) {
          CALLBACK(defData->callbacks->SlotCbk, defrSlotCbkType, &defData->Slot);
          defData->Slot.clear();
        }
      }

slot_rule: '-' K_LAYER { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING 
      {
        if (defData->callbacks->SlotCbk) {
          defData->Slot.setLayer($4);
          defData->Slot.clearPoly();     // free poly, if any
        }
      } geom_slot

geom_slot_rules: // empty 
      | geom_slot_rules geom_slot
      ;

geom_slot: K_RECT pt pt
      {
        if (defData->callbacks->SlotCbk)
          defData->Slot.addRect($2.x, $2.y, $3.x, $3.y);
      }
      | K_POLYGON
      {
          defData->Geometries.Reset();
      }
      firstPt nextPt nextPt otherPts
      {
        if (defData->VersionNum >= 5.6) {  // only 5.6 and beyond
          if (defData->callbacks->SlotCbk)
            defData->Slot.addPolygon(&defData->Geometries);
        }
      }

// 8/31/2001 -  5.4 enhancement 
fill_section: fill_start fill_defs fill_end ;

fill_start: K_FILLS NUMBER ';'
      { if (defData->callbacks->FillStartCbk)
          CALLBACK(defData->callbacks->FillStartCbk, defrFillStartCbkType, ROUND($2)); }

fill_end: K_END K_FILLS
      { if (defData->callbacks->FillEndCbk)
          CALLBACK(defData->callbacks->FillEndCbk, defrFillEndCbkType, 0); }

fill_defs: // empty 
      | fill_defs fill_def
      ;

fill_def: '-' K_LAYER { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->FillCbk) {
            defData->Fill.setLayer($4);
            defData->Fill.clearShapes();    // Free shapes, if any.
        }
      }
      fill_layer_mask_opc_opt geom_fill geom_fill_rules ';'
      {
        if (defData->callbacks->FillCbk) {
            CALLBACK(defData->callbacks->FillCbk, defrFillCbkType, &defData->Fill);
            defData->Fill.Destroy();
            defData->Fill.Init();
        }
      }
      | '-' K_VIA { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING  // 5.7
      {
        if (defData->callbacks->FillCbk) {
          defData->Fill.setVia($4);
          defData->Fill.clearPts();
          defData->Geometries.Reset();
          defData->Orients.clear();
        }
      }
      fill_via_mask_opc_opt firstViaPt otherViaPts ';'
      {
        if (defData->callbacks->FillCbk) {
          defData->Fill.addPts(&defData->Geometries, &defData->Orients);
          CALLBACK(defData->callbacks->FillCbk, defrFillCbkType, &defData->Fill);
        }

        defData->Fill.clear();
        defData->Orients.clear();
      }

geom_fill_rules: // empty 
      | geom_fill_rules geom_fill
      ;

geom_fill: K_RECT pt pt
      {
        if (defData->callbacks->FillCbk) {
            defData->Fill.addRect($2.x, $2.y, $3.x, $3.y);
        }

        if (defData->callbacks->FillPartialCbk
            && defData->Fill.numRectangles() == PARTIAL_CBK_THRESHOLD) {
            CALLBACK(defData->callbacks->FillPartialCbk,
                     defrFillPartialCbkType, &defData->Fill);
            defData->Fill.clearShapes();
        }
      }
      | K_POLYGON
      {
        defData->Geometries.Reset();
      }
      firstPt nextPt nextPt otherPts
      {
        if (defData->VersionNum >= 5.6) {  // only 5.6 and beyond
            if (defData->callbacks->FillCbk) {
                defData->Fill.addPolygon(&defData->Geometries);
            }

            if (defData->callbacks->FillPartialCbk
                && defData->Fill.numPolygons() == PARTIAL_CBK_THRESHOLD) {
                CALLBACK(defData->callbacks->FillPartialCbk,
                         defrFillPartialCbkType, &defData->Fill);
                defData->Fill.clearShapes();
            }
        } else {
            defData->defMsg = (char*)malloc(DEF_MSGS);
            sprintf (defData->defMsg,
              "POLYGON statement in FILLS LAYER is a version 5.6 and later syntax.\nYour def file is defined with version %.2f.", defData->VersionNum);
            defData->defError(6564, defData->defMsg);
            free(defData->defMsg);
            CHKERR();
        }
      }

fill_layer_mask_opc_opt: // empty 
    | fill_layer_mask_opc_opt opt_mask_opc_l
    ;
opt_mask_opc_l: fill_layer_opc
    | fill_mask 
    | fill_layer_prop
    ;
        
// 5.7
fill_layer_opc: 
      '+' K_OPC
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->FillCbk) {
             if (defData->fillWarnings++ < defData->settings->FillWarnings) {
               defData->defMsg = (char*)malloc(10000);
               sprintf (defData->defMsg,
                 "The LAYER OPC is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
               defData->defError(6553, defData->defMsg);
               free(defData->defMsg);
               CHKERR();
             }
           }
        } else {
           if (defData->callbacks->FillCbk)
             defData->Fill.setLayerOpc();
        }
      }

firstViaPt: fill_via_orient pt
          { 
            defData->Geometries.startList($2.x, $2.y); 
            defData->Orients.push_back($1);
          }

nextViaPt: fill_via_orient pt
          { 
            defData->Geometries.addToList($2.x, $2.y); 
            defData->Orients.push_back($1);
          }

otherViaPts: // empty 
        | otherViaPts nextViaPt
        ;

fill_via_orient:
    {
        $$ = 0;
    }
    | orient
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("FILLS ... - VIA ... orient pt  ...")) {
                CHKERR();
            }
        } 

        $$ = $1;
    }

fill_via_mask_opc_opt: // empty 
    | fill_via_mask_opc_opt opt_mask_opc
    ;

opt_mask_opc: fill_via_opc
    | fill_viaMask
    | fill_via_prop
    ;

fill_via_prop: '+' K_PROPERTY  
    { 
        defData->dumb_mode = 2; 
    } 
    prop_name_value
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("FILLS: ... + VIA ... + PROPERTY propName propValue")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->FillCbk) {
                defData->setPropDataType($4, "SPECIALROUTE", defData->session->SpecialRouteProp);
                defData->Fill.addViaProp($4);
                $4 = NULL;
            }
        }

        delete $4;
    }

fill_layer_prop: '+' K_PROPERTY 
    {
        defData->dumb_mode = 2; 
    } 
    prop_name_value
    {
        if (defData->VersionNum < 6.0 - 0.00001) {
            if (defData->def60NewSyntaxError("FILLS: ... + LAYER ... + PROPERTY propName propValue")) {
                CHKERR();
            }
        } else {
            if (defData->callbacks->FillCbk) {
                defData->setPropDataType($4, "SPECIALROUTE", defData->session->SpecialRouteProp);
                defData->Fill.addLayerProp($4);
                $4 = NULL;
            }
        }

        delete $4;
    }

// 5.7
fill_via_opc:
      '+' K_OPC
      {
        if (defData->VersionNum < 5.7) {
           if (defData->callbacks->FillCbk) {
             if (defData->fillWarnings++ < defData->settings->FillWarnings) {
               defData->defMsg = (char*)malloc(10000);
               sprintf (defData->defMsg,
                 "The VIA OPC is available in version 5.7 or later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
               defData->defError(6554, defData->defMsg);
               free(defData->defMsg);
               CHKERR();
             }
           }
        } else {
           if (defData->callbacks->FillCbk)
             defData->Fill.setViaOpc();
        }
      }
      
fill_mask:
      '+' K_MASK NUMBER
      { 
        if (defData->validateMaskInput((int)$3, defData->fillWarnings, defData->settings->FillWarnings)) {
             if (defData->callbacks->FillCbk) {
                defData->Fill.setMask((int)$3);
             }
        }
      }

fill_viaMask:
      '+' K_MASK NUMBER
      { 
        if (defData->validateMaskInput((int)$3, defData->fillWarnings, defData->settings->FillWarnings)) {
             if (defData->callbacks->FillCbk) {
                defData->Fill.setMask((int)$3);
             }
        }
      }
      
// 11/17/2003 - 5.6 enhancement 
nondefaultrule_section: nondefault_start nondefault_def nondefault_defs
    nondefault_end ;

nondefault_start: K_NONDEFAULTRULES NUMBER ';'
      { 
        if (defData->VersionNum < 5.6) {
          if (defData->callbacks->NonDefaultStartCbk) {
            if (defData->nonDefaultWarnings++ < defData->settings->NonDefaultWarnings) {
              defData->defMsg = (char*)malloc(1000);
              sprintf (defData->defMsg,
                 "The NONDEFAULTRULE statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f.", defData->VersionNum);
              defData->defError(6545, defData->defMsg);
              free(defData->defMsg);
              CHKERR();
            }
          }
        } else if (defData->callbacks->NonDefaultStartCbk)
          CALLBACK(defData->callbacks->NonDefaultStartCbk, defrNonDefaultStartCbkType,
                   ROUND($2));
      }

nondefault_end: K_END K_NONDEFAULTRULES
      { if (defData->callbacks->NonDefaultEndCbk)
          CALLBACK(defData->callbacks->NonDefaultEndCbk, defrNonDefaultEndCbkType, 0); }

nondefault_defs: // empty 
      | nondefault_defs nondefault_def
      ;

nondefault_def: '-' { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.clear(); 
          defData->NonDefault.setName($3);
        }
      }
      nondefault_options ';'
      { if (defData->callbacks->NonDefaultCbk)
          CALLBACK(defData->callbacks->NonDefaultCbk, defrNonDefaultCbkType, &defData->NonDefault); }

nondefault_options: // empty  
      | nondefault_options nondefault_option
      ;

nondefault_option: '+' K_HARDSPACING
      {
        if (defData->callbacks->NonDefaultCbk)
          defData->NonDefault.setHardspacing();
      }
      | '+' K_LAYER { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
        K_WIDTH NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addLayer($4);
          defData->NonDefault.addWidth($6);
        }
      }
      nondefault_layer_options
      | '+' K_VIA { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addVia($4);
        }
      }
      | '+' K_VIARULE { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addViaRule($4);
        }
      }
      | '+' K_MINCUTS { defData->dumb_mode = 1; defData->no_num = 1; } T_STRING NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addMinCuts($4, (int)$5);
        }
      }
      | nondefault_prop_opt
      ;

nondefault_layer_options: // empty 
      | nondefault_layer_options nondefault_layer_option

nondefault_layer_option:
      K_DIAGWIDTH NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addDiagWidth($2);
        }
      }
      | K_SPACING NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addSpacing($2);
        }
      }
      | K_WIREEXT NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          defData->NonDefault.addWireExt($2);
        }
      }
      ;

nondefault_prop_opt: '+' K_PROPERTY { defData->dumb_mode = DEF_MAX_INT;  }
                     nondefault_prop_list
      { defData->dumb_mode = 0; }

nondefault_prop_list: // empty 
      | nondefault_prop_list nondefault_prop
      ;

nondefault_prop: T_STRING NUMBER
      {
        if (defData->callbacks->NonDefaultCbk) {
          char propTp;
          char* str = defData->ringCopy("                       ");
          propTp = defData->session->NDefProp.propType($1);
          CHKPROPTYPE(propTp, $1, "NONDEFAULTRULE");
          sprintf(str, "%g", $2);
          defData->NonDefault.addNumProperty($1, $2, str, propTp);
        }
      }
      | T_STRING QSTRING
      {
        if (defData->callbacks->NonDefaultCbk) {
          char propTp;
          propTp = defData->session->NDefProp.propType($1);
          CHKPROPTYPE(propTp, $1, "NONDEFAULTRULE");
          defData->NonDefault.addProperty($1, $2, propTp);
        }
      }
      | T_STRING T_STRING
      {
        if (defData->callbacks->NonDefaultCbk) {
          char propTp;
          propTp = defData->session->NDefProp.propType($1);
          CHKPROPTYPE(propTp, $1, "NONDEFAULTRULE");
          defData->NonDefault.addProperty($1, $2, propTp);
        }
      }

// 12/2/2003 - 5.6 enhancement 
styles_section: styles_start styles_rules styles_end ;

styles_start: K_STYLES NUMBER ';'
      {
        if (defData->VersionNum < 5.6) {
          if (defData->callbacks->StylesStartCbk) {
            if (defData->stylesWarnings++ < defData->settings->StylesWarnings) {
              defData->defMsg = (char*)malloc(1000);
              sprintf (defData->defMsg,
                 "The STYLES statement is available in version 5.6 and later.\nHowever, your DEF file is defined with version %.2f", defData->VersionNum);
              defData->defError(6546, defData->defMsg);
              free(defData->defMsg);
              CHKERR();
            }
          }
        } else if (defData->VersionNum >= 6.0 - 0.00001) {
            if (defData->def60ObsoletedError("STYLES ... END STYLES")) {
                CHKERR();
            }
        } else if (defData->callbacks->StylesStartCbk)
          CALLBACK(defData->callbacks->StylesStartCbk, defrStylesStartCbkType, ROUND($2));
      }

styles_end: K_END K_STYLES
      { if (defData->callbacks->StylesEndCbk)
          CALLBACK(defData->callbacks->StylesEndCbk, defrStylesEndCbkType, 0); }

styles_rules: // empty 
      | styles_rules styles_rule
      ;

styles_rule: '-' K_STYLE NUMBER
      {
        if (defData->callbacks->StylesCbk) defData->Styles.setStyle((int)$3);
        defData->Geometries.Reset();
      }
      firstPt nextPt otherPts ';'
      {
        if (defData->VersionNum >= 5.6) {  // only 5.6 and beyond will call the callback
          if (defData->callbacks->StylesCbk) {
            defData->Styles.setPolygon(&defData->Geometries);
            CALLBACK(defData->callbacks->StylesCbk, defrStylesCbkType, &defData->Styles);
          }
        }
      }
      

%%

END_LEFDEF_PARSER_NAMESPACE
