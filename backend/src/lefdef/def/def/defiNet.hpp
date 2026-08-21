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
//  $Author: icftcm $
//  $Revision: #14 $
//  $Date: 2023/06/26 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#ifndef defiNet_h
#define defiNet_h

#include <stdio.h>
#include "defiKRDefs.hpp"
#include "defiPath.hpp"
#include "defiMisc.hpp"
#include "defiProp.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

class defrData;

/* Return codes for defiNet::viaOrient 
    DEF_ORIENT_N  0
    DEF_ORIENT_W  1
    DEF_ORIENT_S  2
    DEF_ORIENT_E  3
    DEF_ORIENT_FN 4
    DEF_ORIENT_FW 5
    DEF_ORIENT_FS 6
    DEF_ORIENT_FE 7
*/

class defiWire {
public:
  defiWire(defrData *data);
  ~defiWire();

  void Init(const char* type, const char* wireShieldName);
  void Destroy();
  void clear();
  void addPath(defiPath *p, int reset, int netOsnet, int *needCbk);

  const char* wireType() const;
  const char* wireShieldNetName() const;
  int         numPaths() const;
  int         hasShield() const;

  defiPath*   path(int index);
  const defiPath*   path(int index) const;

  void bumpPaths(long long size);

protected:
  char*      type_;
  char*      wireShieldName_;    // It only set from specialnet SHIELD, 5.4
  int        numPaths_;
  long long  pathsAllocated_;
  defiPath** paths_;

  defrData  *defData;
};



class defiSubnet {
public:
  defiSubnet(defrData *data);
  void Init();

  void Destroy();
  ~defiSubnet();

  void setName(const char* name);
  void setNonDefault(const char* name);
  void addPin(const char* instance, const char* pin, int syn);
  void addMustPin(const char* instance, const char* pin, int syn);

  // WMD -- the following will be removed by the next release
  void setType(const char* typ);  // Either FIXED COVER ROUTED
  void addPath(defiPath* p, int reset, int netOsnet, int *needCbk);

  // NEW: a net can have more than 1 wire
  void addWire(defiWire *wire);

  // Debug printing
  void print(FILE* f) const;

  const char* name() const;
  int numConnections() const;
  const char* instance(int index) const;
  const char* pin(int index) const;
  int pinIsSynthesized(int index) const;
  int pinIsMustJoin(int index) const;

  // WMD -- the following will be removed by the next release
  int isFixed() const;
  int isRouted() const;
  int isCover() const;

  int hasNonDefaultRule() const;

  // WMD -- the following will be removed by the next release
  int numPaths() const;
  defiPath* path(int index);
  const defiPath* path(int index) const;

  const char* nonDefaultRule() const;

  int         numWires() const;
  defiWire*   wire(int index);
  const defiWire*   wire(int index) const;

  void bumpName(long long size);
  void bumpPins(long long  size);
  void bumpPaths(long long  size);
  void clear();

protected:
  char*         name_;            // name.
  int           nameSize_;          // allocated size of name.
  int           numPins_;           // number of pins used in array.
  long long     pinsAllocated_;     // number of pins allocated in array.
  char**        instances_;      // instance names for connections
  char**        pins_;           // pin names for connections
  char*         synthesized_;     // synthesized flags for pins
  char*         musts_;           // must-join flags

  // WMD -- the following will be removed by the next release
  char       isFixed_;        // net type
  char       isRouted_;
  char       isCover_;
  defiPath** paths_;          // paths for this subnet
  int        numPaths_;       // number of paths used
  long long  pathsAllocated_; // allocated size of paths array

  int        numWires_;          // number of wires defined in the subnet
  long long  wiresAllocated_;    // number of wires allocated in the subnet
  defiWire** wires_;             // this replace the paths
  char*      nonDefaultRule_;

  defrData *defData;
};



class defiVpin {
public:
  defiVpin(defrData *data);
  ~defiVpin();

  void Init(const char* name);
  void Destroy();
  void setLayer(const char* name);
  void setBounds(int xl, int yl, int xh, int yh);
  void setOrient(int orient);
  void setLoc(int x, int y);
  void setStatus(char st);

  int xl() const ;
  int yl() const ;
  int xh() const ;
  int yh() const ;
  char status() const;      /* P-placed, F-fixed, C-cover, ' ' - not set */
  int orient() const ;
  const char* orientStr() const ;
  int xLoc() const;
  int yLoc() const;
  const char* name() const;
  const char* layer() const;

protected:
  int xl_;
  int yl_;
  int xh_;
  int yh_;
  int orient_;  /* 0-7  -1 is no orient */
  char status_; /* P-placed  F-fixed  C-cover  ' '- none */
  int xLoc_;
  int yLoc_;
  char* name_;
  char* layer_;

  defrData *defData;
};



// Pre 5.4
class defiShield {
public:
  defiShield(defrData *data);
  ~defiShield();

  void Init(const char* name);
  void Destroy();
  void clear();
  void addPath(defiPath *p, int reset, int netOsnet, int *needCbk);

  const char* shieldName() const;
  int         numPaths() const;

  defiPath*         path(int index);
  const defiPath*   path(int index) const;

  void bumpPaths(long long size);

protected:
  char*      name_;
  int        numPaths_;
  long long  pathsAllocated_;
  defiPath** paths_;

  defrData *defData;
};



// *****************************************************************************
// defiNetPoly
// *****************************************************************************
class defiNetPoly {
public:
                            defiNetPoly(char            *name,
                                        defiGeometries  *geom,
                                        int             mask,
                                        char            *routeStatus,
                                        char            *routeStatusShieldName,
                                        char            *shapeType,
                                        defrProps       *props);
                            ~defiNetPoly();

    const char              *getName() const;
    defiPoints              getPoints() const;
    int                     getMask() const;
    const char              *getRouteStatus() const;
    const char              *getRouteStatusShieldName() const;
    const char              *getShapeType() const;
    const defrProps         *getProps() const;

protected:
    char                    *m_name;
    defiPoints              m_points;
    int                     m_mask;
    char                    *m_routeStatus;
    char                    *m_routeStatusShieldName;
    char                    *m_shapeType;
    defrProps               *m_props;
};



// *****************************************************************************
// defiNetRect
// *****************************************************************************
class defiNetRect {
public:
                            defiNetRect(char      *name,
                                        int       xl,
                                        int       yl,
                                        int       xh,
                                        int       yh,
                                        int       mask,
                                        char      *routeStatus,
                                        char      *routeStatusShieldName,
                                        char      *shapeType,
                                        defrProps *props);
                            ~defiNetRect();

    const char              *getName() const;
    int                     getXl() const;
    int                     getYl() const;
    int                     getXh() const;
    int                     getYh() const;
    int                     getMask() const;
    const char              *getRouteStatus() const;
    const char              *getRouteStatusShieldName() const;
    const char              *getShapeType() const;
    const defrProps         *getProps() const;

protected:
    char                    *m_name;
    int                     m_xl;
    int                     m_yl;
    int                     m_xh;
    int                     m_yh;
    int                     m_mask;
    char                    *m_routeStatus;
    char                    *m_routeStatusShieldName;
    char                    *m_shapeType;
    defrProps               *m_props;
};



// *****************************************************************************
// defiNetVia
// *****************************************************************************
class defiNetVia {
public:
                            defiNetVia(char           *name,
                                       int            orient,
                                       defiGeometries *geom,
                                       int            mask,
                                       char           *routeStatus,
                                       char           *routeStatusShieldName,
                                       char           *shapeType,
                                       defrProps      *props);
                            ~defiNetVia();

    const char              *getName() const;
    int                     getOrient() const;
    defiPoints              getPoints() const;
    int                     getTopMaskNum() const;
    int                     getCutMaskNum() const;
    int                     getBottomMaskNum() const;
    const char              *getRouteStatus() const;
    const char              *getRouteStatusShieldName() const;
    const char              *getShapeType() const;
    const defrProps         *getProps() const;

protected:
    char                    *m_name;
    int                     m_orient;
    defiPoints              m_points;
    int                     m_mask;
    char                    *m_routeStatus;
    char                    *m_routeStatusShieldName;
    char                    *m_shapeType;
    defrProps               *m_props;
};



// *****************************************************************************
// defiNet
// *****************************************************************************
class defiNet {
public:
  defiNet(defrData *data);
  void Init();

  void Destroy();
  ~defiNet();

  // Routines used by YACC to set the fields in the net.
  void setName(const char* name);
  void addPin(const char* instance, const char* pin, int syn);
  void addMustPin(const char* instance, const char* pin, int syn);
  void setWeight(int w);

  // WMD -- the following will be removed by the next release
  void setType(const char* typ);  // Either FIXED COVER ROUTED

  void addProp(const char* name, const char* value, char type);
  void addNumProp(const char* name, double d,
                  const char* value, char type);
  void addSubnet(defiSubnet* subnet);
  // NEW: a net can have more than 1 wire
  void addWire(defiWire *wire);
  void addShape(const char *shapeType);         // 5.8
  void setSource(const char* typ);
  void setFixedbump();                          // 5.4.1
  void setFrequency(double frequency);          // 5.4.1
  void setOriginal(const char* typ);
  void setPattern(const char* typ);
  void setCap(double w);
  void setUse(const char* typ);
  void setNonDefaultRule(const char* typ);
  void setStyle(int style);
  void addShield(defiShield *shield, bool isNoShield);    // pre 5.4
  void addShieldNet(const char* shieldNetName);

  void clear();
  void setWidth(const char* layer, double dist);
  void setSpacing(const char* layer, double dist);
  void setVoltage(double num);
  void setRange(double left, double right);
  void setXTalk(int num);
  void addVpin(const char* name);
  void addVpinLayer(const char* name);
  void addVpinLoc(const char* status, int x, int y, int orient);
  void addVpinBounds(int xl, int yl, int xh, int yh);

  void                      addPolygon(defiNetPoly    *poly,
                                       int            *needCbk);
  void                      addRect(defiNetRect       *rect,
                                    int               *needCbk);
  void                      addVia(defiNetVia         *via,
                                   int                *needCbk);

  // For OA to modify the netName, id & pinName
  void changeNetName(const char* name);
  void changeInstance(const char* name, int index);
  void changePin(const char* name, int index);

  // Routines to return the value of net data.
  const char*  name() const;
  int          weight() const;
  int          numProps() const;
  const char*  propName(int index) const;
  const char*  propValue(int index) const;
  double propNumber(int index) const;
  char   propType(int index) const;
  int    propIsNumber(int index) const;
  int    propIsString(int index) const;
  int          numConnections() const;
  const char*  instance(int index) const;
  const char*  pin(int index) const;
  int          pinIsMustJoin(int index) const;
  int          pinIsSynthesized(int index) const;
  int          numSubnets() const;

  defiSubnet*  subnet(int index);
  const defiSubnet*  subnet(int index) const;

  // WMD -- the following will be removed by the next release
  int         isFixed() const;
  int         isRouted() const;
  int         isCover() const;

  /* The following routines are for wiring */
  int         numWires() const;

  defiWire*   wire(int index);
  const defiWire*   wire(int index) const;

  /* Routines to get the information about Virtual Pins. */
  int       numVpins() const;
  
  defiVpin* vpin(int index);
  const defiVpin* vpin(int index) const;

  int hasProps() const;
  int hasWeight() const;
  int hasSubnets() const;
  int hasSource() const;
  int hasFixedbump() const;                          // 5.4.1
  int hasFrequency() const;                          // 5.4.1
  int hasPattern() const;
  int hasOriginal() const;
  int hasCap() const;
  int hasUse() const;
  int hasStyle() const;
  int hasNonDefaultRule() const;
  int hasVoltage() const;
  int hasSpacingRules() const;
  int hasWidthRules() const;
  int hasXTalk() const;

  int numSpacingRules() const;
  void spacingRule(int index, char** layer, double* dist, double* left,
                   double* right) const;
  int numWidthRules() const;
  void widthRule(int index, char** layer, double* dist) const;
  double voltage() const;

  int            XTalk() const;
  const char*    source() const;
  double         frequency() const;
  const char*    original() const;
  const char*    pattern() const;
  double         cap() const;
  const char*    use() const;
  int            style() const;
  const char*    nonDefaultRule() const;

  // WMD -- the following will be removed by the next release
  int            numPaths() const;
  
  defiPath*            path(int index);
  const defiPath*      path(int index) const;

  int            numShields() const;          // pre 5.4

  defiShield*    shield(int index);           // pre 5.4
  const defiShield*    shield(int index) const ;           // pre 5.4

  int            numShieldNets() const;
  const char*    shieldNet(int index) const;
  int            numNoShields() const;        // pre 5.4

  defiShield*    noShield(int index);         // pre 5.4
  const defiShield*    noShield(int index) const;         // pre 5.4

  int                       numPolygons() const;
  defiNetPoly               *getPoly(int index) const;
  const  char               *polygonName(int index) const;
  defiPoints                getPolygon(int index) const;
  int                       polyMask(int index) const;
  const char                *polyRouteStatus(int index) const;
  int                       hasPolyRouteStatusShield(int index) const;
  const char                *polyRouteStatusShieldName(int index) const;
  const char                *polyShapeType(int index) const;
  int                       numPolyProps(int index) const;
  const defiProp            *polyProp(int index,
                                      int propIdx) const;


  int                       numRectangles() const;
  defiNetRect               *getRect(int index) const;
  const  char               *rectName(int index) const;
  int                       xl(int index)const;
  int                       yl(int index)const;
  int                       xh(int index)const;
  int                       yh(int index)const;
  int                       rectMask(int index)const;
  const char                *rectRouteStatus(int index) const;
  int                       hasRectRouteStatusShield(int index) const;
  const char                *rectRouteStatusShieldName(int index) const;
  const char                *rectShapeType(int index) const;
  int                       numRectProps(int index) const;
  const defiProp            *rectProp(int index,
                                      int propIdx) const;

  int                       numViaSpecs() const;
  defiNetVia                *getVia(int index) const;
  defiPoints                getViaPts(int index) const;                       
  const char                *viaName(int index) const;
  int                       viaOrient(int index) const;
  const char                *viaOrientStr(int index) const;
  int                       topMaskNum(int index) const;
  int                       cutMaskNum(int index) const;
  int                       bottomMaskNum(int index) const;
  const char                *viaRouteStatus(int index) const;
  int                       hasViaRouteStatusShield(int index) const;
  const char                *viaRouteStatusShieldName(int index) const;
  const char                *viaShapeType(int index) const;
  int                       numViaProps(int index) const;
  const defiProp            *viaProp(int index,
                                     int propIdx) const;

  // Debug printing
  void print(FILE* f) const;


  void bumpName(long long size);
  void bumpPins(long long size);
  void bumpProps(long long size);
  void bumpSubnets(long long size);
  void bumpPaths(long long  size);
  void bumpShieldNets(long long size);

  // The method freeWire() is added is user select to have a callback
  // per wire within a net This is an internal method and is not public
  void freeWire();
  void freeShield();

  // Clear the rectangles & polygons data if partial path callback is set
  void clearRectPolyNPath();
  void clearRectPoly();
  void clearVia();


protected:
  char*     name_;          // name.
  int       nameSize_;      // allocated size of name.
  int       numPins_;       // number of pins used in array.
  long long pinsAllocated_; // number of pins allocated in array.
  char**    instances_;     // instance names for connections
  char**    pins_;          // pin names for connections
  char*     musts_;         // must-join flags for pins
  char*     synthesized_;   // synthesized flags for pins
  int       weight_;        // net weight
  char      hasWeight_;     // flag for optional weight

  // WMD -- the following will be removed by the nex release
  char isFixed_;        // net type
  char isRouted_;
  char isCover_;

  char hasCap_;         // file supplied a capacitance value
  char hasFrequency_;   // file supplied a frequency value
  char hasVoltage_;
  int numProps_;        // num of props in array
  char**  propNames_;   // Prop names
  char**  propValues_;  // Prop values All in strings!
  double* propDValues_; // Prop values in numbers!
  char*   propTypes_;   // Prop types, 'I' - Integer, 'R' - Real, 'S' - String

  long long    propsAllocated_;   // allocated size of props array
  int          numSubnets_;       // num of subnets in array
  defiSubnet** subnets_;          // Prop names
  long long    subnetsAllocated_; // allocated size of props array
  double       cap_;              // cap value
  char*        source_;
  int          fixedbump_;     // 5.4.1
  double       frequency_;     // 5.4.1
  char* pattern_;
  char* original_;
  char* use_;
  char* nonDefaultRule_;
  int   style_;

  // WMD -- the following will be removed by the nex release
  defiPath** paths_;          // paths for this subnet
  int        numPaths_;       // number of paths used
  long long  pathsAllocated_; // allocated size of paths array

  double voltage_;

  int         numWires_;         // number of wires defined in the net
  long long   wiresAllocated_;   // allocated size of wire paths array
  defiWire**  wires_;            // this replace the paths

  long long   widthsAllocated_;
  int         numWidths_;
  char**      wlayers_;
  double*     wdist_;

  long long   spacingAllocated_;
  int         numSpacing_;
  char**      slayers_;
  double*     sdist_;
  double*     sleft_;
  double*     sright_;
  int         xTalk_;

  int         numVpins_;
  long long   vpinsAllocated_;
  defiVpin**  vpins_;

  int          numShields_;            // number of SHIELD paths used
  long long    shieldsAllocated_;      // allocated size of SHIELD paths array
  defiShield** shields_;               // SHIELD data 
  int          numNoShields_;          // number of NOSHIELD paths used

  int          numShieldNet_;          // number of SHIELDNETS used in array.
  long long    shieldNetsAllocated_;   // number of SHIELDNETS allocated in array.
  char**       shieldNet_;             // name of the SHIELDNET

  int                       numPolys_;
  int                       polysAllocated_;
  defiNetPoly               **polys_;

  int                       numRects_;
  int                       rectsAllocated_;
  defiNetRect               **rects_;

  int                       numVias_;
  int                       viasAllocated_;
  defiNetVia                **vias_;

  defrData*                 defData;
};



// *****************************************************************************
// Const getters of the defiNetPoly class.
// *****************************************************************************
inline const char*
defiNetPoly::getName() const
{
    return m_name;
}

inline defiPoints
defiNetPoly::getPoints() const
{
    return m_points;
}

inline int
defiNetPoly::getMask() const
{
    return m_mask;
}

inline const char*
defiNetPoly::getRouteStatus() const
{
    return m_routeStatus;
}

inline const char*
defiNetPoly::getRouteStatusShieldName() const
{
    return m_routeStatusShieldName;
}

inline const char*
defiNetPoly::getShapeType() const
{
    return m_shapeType;
}

inline const defrProps*
defiNetPoly::getProps() const
{
    return m_props;
}



// *****************************************************************************
// Const getters of the defiNetRect class.
// *****************************************************************************
inline const char*
defiNetRect::getName() const
{
    return m_name;
}

inline int
defiNetRect::getXl() const
{
    return m_xl;
}

inline int
defiNetRect::getYl() const
{
    return m_yl;
}

inline int
defiNetRect::getXh() const
{
    return m_xh;
}

inline int
defiNetRect::getYh() const
{
    return m_yh;
}

inline int
defiNetRect::getMask() const
{
    return m_mask;
}

inline const char*
defiNetRect::getRouteStatus() const
{
    return m_routeStatus;
}

inline const char*
defiNetRect::getRouteStatusShieldName() const
{
    return m_routeStatusShieldName;
}

inline const char*
defiNetRect::getShapeType() const
{
    return m_shapeType;
}

inline const defrProps*
defiNetRect::getProps() const
{
    return m_props;
}



// *****************************************************************************
// Const getters of the defiNetVia class.
// *****************************************************************************
inline const char*
defiNetVia::getName() const
{
    return m_name;
}

inline defiPoints
defiNetVia::getPoints() const
{
    return m_points;
}

inline int
defiNetVia::getOrient() const
{
    return m_orient;
}

inline int
defiNetVia::getTopMaskNum() const
{
    return m_mask / 100;
}

inline int
defiNetVia::getCutMaskNum() const
{
    return (m_mask / 10) % 10;
}

inline int
defiNetVia::getBottomMaskNum() const
{
    return m_mask % 10;
}

inline const char*
defiNetVia::getRouteStatus() const
{
    return m_routeStatus;
}

inline const char*
defiNetVia::getRouteStatusShieldName() const
{
    return m_routeStatusShieldName;
}

inline const char*
defiNetVia::getShapeType() const
{
    return m_shapeType;
}

inline const defrProps*
defiNetVia::getProps() const
{
    return m_props;
}

END_LEFDEF_PARSER_NAMESPACE

USE_LEFDEF_PARSER_NAMESPACE

#endif
