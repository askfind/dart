/*
 * Copyright (c) 2017, Graphics Lab, Georgia Tech Research Corporation
 * Copyright (c) 2017, Personal Robotics Lab, Carnegie Mellon University
 * All rights reserved.
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#include "SharedLibraryWamIkFast.hpp"

#include "dart/external/ikfast/ikfast.h" // found inside share/openrave-X.Y/python/ikfast.h
using namespace ikfast;

// check if the included ikfast version matches what this file was compiled with
#define IKFAST_COMPILE_ASSERT(x) extern int __dummy[(int)x]
IKFAST_COMPILE_ASSERT(IKFAST_VERSION==71);

#include <cmath>
#include <vector>
#include <limits>
#include <algorithm>
#include <complex>

#define IKFAST_STRINGIZE2(s) #s
#define IKFAST_STRINGIZE(s) IKFAST_STRINGIZE2(s)

#ifndef IKFAST_ASSERT
#include <stdexcept>
#include <sstream>
#include <iostream>

#ifdef _MSC_VER
#ifndef __PRETTY_FUNCTION__
#define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif
#endif

#ifndef __PRETTY_FUNCTION__
#define __PRETTY_FUNCTION__ __func__
#endif

#define IKFAST_ASSERT(b) { if( !(b) ) { std::stringstream ss; ss << "ikfast exception: " << __FILE__ << ":" << __LINE__ << ": " <<__PRETTY_FUNCTION__ << ": Assertion '" << #b << "' failed"; throw std::runtime_error(ss.str()); } }

#endif

#if defined(_MSC_VER)
#define IKFAST_ALIGNED16(x) __declspec(align(16)) x
#else
#define IKFAST_ALIGNED16(x) x __attribute((aligned(16)))
#endif

#define IK2PI  ((IkReal)6.28318530717959)
#define IKPI  ((IkReal)3.14159265358979)
#define IKPI_2  ((IkReal)1.57079632679490)

#ifdef _MSC_VER
#ifndef isnan
#define isnan _isnan
#endif
#ifndef isinf
#define isinf _isinf
#endif
//#ifndef isfinite
//#define isfinite _isfinite
//#endif
#endif // _MSC_VER

// lapack routines
extern "C" {
  void dgetrf_ (const int* m, const int* n, double* a, const int* lda, int* ipiv, int* info);
  void zgetrf_ (const int* m, const int* n, std::complex<double>* a, const int* lda, int* ipiv, int* info);
  void dgetri_(const int* n, const double* a, const int* lda, int* ipiv, double* work, const int* lwork, int* info);
  void dgesv_ (const int* n, const int* nrhs, double* a, const int* lda, int* ipiv, double* b, const int* ldb, int* info);
  void dgetrs_(const char *trans, const int *n, const int *nrhs, double *a, const int *lda, int *ipiv, double *b, const int *ldb, int *info);
  void dgeev_(const char *jobvl, const char *jobvr, const int *n, double *a, const int *lda, double *wr, double *wi,double *vl, const int *ldvl, double *vr, const int *ldvr, double *work, const int *lwork, int *info);
}

using namespace std; // necessary to get std math routines

#ifdef IKFAST_NAMESPACE
namespace IKFAST_NAMESPACE {
#endif

inline float IKabs(float f) { return fabsf(f); }
inline double IKabs(double f) { return fabs(f); }

inline float IKsqr(float f) { return f*f; }
inline double IKsqr(double f) { return f*f; }

inline float IKlog(float f) { return logf(f); }
inline double IKlog(double f) { return log(f); }

// allows asin and acos to exceed 1
#ifndef IKFAST_SINCOS_THRESH
#define IKFAST_SINCOS_THRESH ((IkReal)2e-6)
#endif

// used to check input to atan2 for degenerate cases
#ifndef IKFAST_ATAN2_MAGTHRESH
#define IKFAST_ATAN2_MAGTHRESH ((IkReal)2e-6)
#endif

// minimum distance of separate solutions
#ifndef IKFAST_SOLUTION_THRESH
#define IKFAST_SOLUTION_THRESH ((IkReal)1e-6)
#endif

// there are checkpoints in ikfast that are evaluated to make sure they are 0. This threshold speicfies by how much they can deviate
#ifndef IKFAST_EVALCOND_THRESH
#define IKFAST_EVALCOND_THRESH ((IkReal)0.000005)
#endif


inline float IKasin(float f)
{
IKFAST_ASSERT( f > -1-IKFAST_SINCOS_THRESH && f < 1+IKFAST_SINCOS_THRESH ); // any more error implies something is wrong with the solver
if( f <= -1 ) return float(-IKPI_2);
else if( f >= 1 ) return float(IKPI_2);
return asinf(f);
}
inline double IKasin(double f)
{
IKFAST_ASSERT( f > -1-IKFAST_SINCOS_THRESH && f < 1+IKFAST_SINCOS_THRESH ); // any more error implies something is wrong with the solver
if( f <= -1 ) return -IKPI_2;
else if( f >= 1 ) return IKPI_2;
return asin(f);
}

// return positive value in [0,y)
inline float IKfmod(float x, float y)
{
    while(x < 0) {
        x += y;
    }
    return fmodf(x,y);
}

// return positive value in [0,y)
inline double IKfmod(double x, double y)
{
    while(x < 0) {
        x += y;
    }
    return fmod(x,y);
}

inline float IKacos(float f)
{
IKFAST_ASSERT( f > -1-IKFAST_SINCOS_THRESH && f < 1+IKFAST_SINCOS_THRESH ); // any more error implies something is wrong with the solver
if( f <= -1 ) return float(IKPI);
else if( f >= 1 ) return float(0);
return acosf(f);
}
inline double IKacos(double f)
{
IKFAST_ASSERT( f > -1-IKFAST_SINCOS_THRESH && f < 1+IKFAST_SINCOS_THRESH ); // any more error implies something is wrong with the solver
if( f <= -1 ) return IKPI;
else if( f >= 1 ) return 0;
return acos(f);
}
inline float IKsin(float f) { return sinf(f); }
inline double IKsin(double f) { return sin(f); }
inline float IKcos(float f) { return cosf(f); }
inline double IKcos(double f) { return cos(f); }
inline float IKtan(float f) { return tanf(f); }
inline double IKtan(double f) { return tan(f); }
inline float IKsqrt(float f) { if( f <= 0.0f ) return 0.0f; return sqrtf(f); }
inline double IKsqrt(double f) { if( f <= 0.0 ) return 0.0; return sqrt(f); }
inline float IKatan2Simple(float fy, float fx) {
    return atan2f(fy,fx);
}
inline float IKatan2(float fy, float fx) {
    if( isnan(fy) ) {
        IKFAST_ASSERT(!isnan(fx)); // if both are nan, probably wrong value will be returned
        return float(IKPI_2);
    }
    else if( isnan(fx) ) {
        return 0;
    }
    return atan2f(fy,fx);
}
inline double IKatan2Simple(double fy, double fx) {
    return atan2(fy,fx);
}
inline double IKatan2(double fy, double fx) {
    if( std::isnan(fy) ) {
        IKFAST_ASSERT(!std::isnan(fx)); // if both are nan, probably wrong value will be returned
        return IKPI_2;
    }
    else if( std::isnan(fx) ) {
        return 0;
    }
    return atan2(fy,fx);
}

template <typename T>
struct CheckValue
{
    T value;
    bool valid;
};

template <typename T>
inline CheckValue<T> IKatan2WithCheck(T fy, T fx, T epsilon)
{
    CheckValue<T> ret;
    ret.valid = false;
    ret.value = 0;
    if( !std::isnan(fy) && !std::isnan(fx) ) {
        if( IKabs(fy) >= IKFAST_ATAN2_MAGTHRESH || IKabs(fx) > IKFAST_ATAN2_MAGTHRESH ) {
            ret.value = IKatan2Simple(fy,fx);
            ret.valid = true;
        }
    }
    return ret;
}

inline float IKsign(float f) {
    if( f > 0 ) {
        return float(1);
    }
    else if( f < 0 ) {
        return float(-1);
    }
    return 0;
}

inline double IKsign(double f) {
    if( f > 0 ) {
        return 1.0;
    }
    else if( f < 0 ) {
        return -1.0;
    }
    return 0;
}

template <typename T>
inline CheckValue<T> IKPowWithIntegerCheck(T f, int n)
{
    CheckValue<T> ret;
    ret.valid = true;
    if( n == 0 ) {
        ret.value = 1.0;
        return ret;
    }
    else if( n == 1 )
    {
        ret.value = f;
        return ret;
    }
    else if( n < 0 )
    {
        if( f == 0 )
        {
            ret.valid = false;
            ret.value = (T)1.0e30;
            return ret;
        }
        if( n == -1 ) {
            ret.value = T(1.0)/f;
            return ret;
        }
    }

    int num = n > 0 ? n : -n;
    if( num == 2 ) {
        ret.value = f*f;
    }
    else if( num == 3 ) {
        ret.value = f*f*f;
    }
    else {
        ret.value = 1.0;
        while(num>0) {
            if( num & 1 ) {
                ret.value *= f;
            }
            num >>= 1;
            f *= f;
        }
    }

    if( n < 0 ) {
        ret.value = T(1.0)/ret.value;
    }
    return ret;
}

/// solves the forward kinematics equations.
/// \param pfree is an array specifying the free joints of the chain.
IKFAST_API void ComputeFk(const IkReal* j, IkReal* eetrans, IkReal* eerot) {
IkReal x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,x12,x13,x14,x15,x16,x17,x18,x19,x20,x21,x22,x23,x24,x25,x26,x27,x28,x29,x30,x31,x32,x33,x34,x35,x36,x37,x38,x39,x40,x41,x42,x43,x44,x45,x46,x47,x48,x49,x50,x51,x52,x53,x54,x55,x56,x57,x58,x59,x60,x61,x62,x63;
x0=IKsin(j[6]);
x1=IKcos(j[4]);
x2=IKsin(j[0]);
x3=IKcos(j[2]);
x4=(x2*x3);
x5=((1.0)*x4);
x6=IKcos(j[1]);
x7=IKsin(j[2]);
x8=IKcos(j[0]);
x9=(x7*x8);
x10=((1.0)*x9);
x11=((((-1.0)*(1.0)*x5))+(((-1.0)*(1.0)*x10*x6)));
x12=IKsin(j[4]);
x13=IKsin(j[1]);
x14=IKsin(j[3]);
x15=(x13*x14*x8);
x16=((1.0)*x15);
x17=IKcos(j[3]);
x18=((1.0)*x17);
x19=(x2*x7);
x20=((1.0)*x19);
x21=(x3*x8);
x22=(x21*x6);
x23=(x22+(((-1.0)*(1.0)*x20)));
x24=(((x12*(((((-1.0)*(1.0)*x18*x23))+x16))))+((x1*x11)));
x25=IKcos(j[6]);
x26=IKsin(j[5]);
x27=(x13*x17*x8);
x28=((1.0)*x27);
x29=(x14*((x20+(((-1.0)*(1.0)*x22)))));
x30=(x26*((x29+(((-1.0)*(1.0)*x28)))));
x31=IKcos(j[5]);
x32=(((x11*x12))+((x1*(((((-1.0)*(1.0)*x16))+((x17*x23)))))));
x33=(x31*x32);
x34=((0.045)*x19);
x35=((0.55)*x13);
x36=((0.045)*x22);
x37=((((-1.0)*(1.0)*x20*x6))+x21);
x38=(x13*x14*x2);
x39=((1.0)*x38);
x40=(x4*x6);
x41=(x9+x40);
x42=(((x12*((x39+(((-1.0)*(1.0)*x18*x41))))))+((x1*x37)));
x43=(x13*x17*x2);
x44=((1.0)*x43);
x45=(x14*(((((-1.0)*(1.0)*x10))+(((-1.0)*(1.0)*x5*x6)))));
x46=(x26*(((((-1.0)*(1.0)*x44))+x45)));
x47=(((x1*(((((-1.0)*(1.0)*x39))+((x17*x41))))))+((x12*x37)));
x48=(x31*x47);
x49=((0.045)*x9);
x50=((0.045)*x40);
x51=(x13*x7);
x52=(x14*x6);
x53=((1.0)*x52);
x54=(x13*x3);
x55=(x18*x54);
x56=(((x1*x51))+((x12*((x55+x53)))));
x57=(x17*x6);
x58=((1.0)*x57);
x59=(x14*x54);
x60=(x26*(((((-1.0)*(1.0)*x58))+x59)));
x61=(((x1*(((((-1.0)*(1.0)*x55))+(((-1.0)*(1.0)*x53))))))+((x12*x51)));
x62=(x31*x61);
x63=((0.045)*x54);
eerot[0]=(((x0*x24))+((x25*((x30+x33)))));
eerot[1]=(((x0*(((((-1.0)*(1.0)*x30))+(((-1.0)*(1.0)*x33))))))+((x24*x25)));
eerot[2]=(((x26*x32))+((x31*((x28+(((-1.0)*(1.0)*x29)))))));
eetrans[0]=((0.22)+((x17*((x34+(((-1.0)*(1.0)*x36))))))+x36+(((0.045)*x15))+((x14*(((((0.3)*x22))+(((-1.0)*(0.3)*x19))))))+(((0.3)*x27))+(((-1.0)*(1.0)*x34))+((x35*x8)));
eerot[3]=(((x25*((x48+x46))))+((x0*x42)));
eerot[4]=(((x0*(((((-1.0)*(1.0)*x48))+(((-1.0)*(1.0)*x46))))))+((x25*x42)));
eerot[5]=(((x26*x47))+((x31*(((((-1.0)*(1.0)*x45))+x44)))));
eetrans[1]=((0.14)+((x2*x35))+(((0.3)*x43))+(((0.045)*x38))+((x17*(((((-1.0)*(1.0)*x49))+(((-1.0)*(1.0)*x50))))))+x49+x50+((x14*(((((0.3)*x40))+(((0.3)*x9)))))));
eerot[6]=(((x0*x56))+((x25*((x60+x62)))));
eerot[7]=(((x0*(((((-1.0)*(1.0)*x60))+(((-1.0)*(1.0)*x62))))))+((x25*x56)));
eerot[8]=(((x26*x61))+((x31*(((((-1.0)*(1.0)*x59))+x58)))));
eetrans[2]=((0.346)+(((0.045)*x52))+(((-1.0)*(0.3)*x59))+(((0.55)*x6))+(((0.3)*x57))+(((-1.0)*(1.0)*x63))+((x17*x63)));
}

IKFAST_API int GetNumFreeParameters() { return 1; }
IKFAST_API int* GetFreeParameters() { static int freeparams[] = {2}; return freeparams; }
IKFAST_API int GetNumJoints() { return 7; }

IKFAST_API int GetIkRealSize() { return sizeof(IkReal); }

IKFAST_API int GetIkType() { return 0x67000001; }

class IKSolver {
public:
IkReal j4,cj4,sj4,htj4,j4mul,j6,cj6,sj6,htj6,j6mul,j9,cj9,sj9,htj9,j9mul,j10,cj10,sj10,htj10,j10mul,j11,cj11,sj11,htj11,j11mul,j12,cj12,sj12,htj12,j12mul,j8,cj8,sj8,htj8,new_r00,r00,rxp0_0,new_r01,r01,rxp0_1,new_r02,r02,rxp0_2,new_r10,r10,rxp1_0,new_r11,r11,rxp1_1,new_r12,r12,rxp1_2,new_r20,r20,rxp2_0,new_r21,r21,rxp2_1,new_r22,r22,rxp2_2,new_px,px,npx,new_py,py,npy,new_pz,pz,npz,pp;
unsigned char _ij4[2], _nj4,_ij6[2], _nj6,_ij9[2], _nj9,_ij10[2], _nj10,_ij11[2], _nj11,_ij12[2], _nj12,_ij8[2], _nj8;

IkReal j100, cj100, sj100;
unsigned char _ij100[2], _nj100;
bool ComputeIk(const IkReal* eetrans, const IkReal* eerot, const IkReal* pfree, IkSolutionListBase<IkReal>& solutions) {
j4=numeric_limits<IkReal>::quiet_NaN(); _ij4[0] = -1; _ij4[1] = -1; _nj4 = -1; j6=numeric_limits<IkReal>::quiet_NaN(); _ij6[0] = -1; _ij6[1] = -1; _nj6 = -1; j9=numeric_limits<IkReal>::quiet_NaN(); _ij9[0] = -1; _ij9[1] = -1; _nj9 = -1; j10=numeric_limits<IkReal>::quiet_NaN(); _ij10[0] = -1; _ij10[1] = -1; _nj10 = -1; j11=numeric_limits<IkReal>::quiet_NaN(); _ij11[0] = -1; _ij11[1] = -1; _nj11 = -1; j12=numeric_limits<IkReal>::quiet_NaN(); _ij12[0] = -1; _ij12[1] = -1; _nj12 = -1;  _ij8[0] = -1; _ij8[1] = -1; _nj8 = 0;
for(int dummyiter = 0; dummyiter < 1; ++dummyiter) {
    solutions.Clear();
j8=pfree[0]; cj8=cos(pfree[0]); sj8=sin(pfree[0]), htj8=tan(pfree[0]*0.5);
r00 = eerot[0*3+0];
r01 = eerot[0*3+1];
r02 = eerot[0*3+2];
r10 = eerot[1*3+0];
r11 = eerot[1*3+1];
r12 = eerot[1*3+2];
r20 = eerot[2*3+0];
r21 = eerot[2*3+1];
r22 = eerot[2*3+2];
px = eetrans[0]; py = eetrans[1]; pz = eetrans[2];

new_r00=r00;
new_r01=r01;
new_r02=r02;
new_px=((-0.22)+px);
new_r10=r10;
new_r11=r11;
new_r12=r12;
new_py=((-0.14)+py);
new_r20=r20;
new_r21=r21;
new_r22=r22;
new_pz=((-0.346)+pz);
r00 = new_r00; r01 = new_r01; r02 = new_r02; r10 = new_r10; r11 = new_r11; r12 = new_r12; r20 = new_r20; r21 = new_r21; r22 = new_r22; px = new_px; py = new_py; pz = new_pz;
IkReal x64=((1.0)*py);
IkReal x65=((1.0)*pz);
IkReal x66=((1.0)*px);
pp=((pz*pz)+(py*py)+(px*px));
npx=(((pz*r20))+((py*r10))+((px*r00)));
npy=(((pz*r21))+((py*r11))+((px*r01)));
npz=(((px*r02))+((pz*r22))+((py*r12)));
rxp0_0=(((pz*r10))+(((-1.0)*r20*x64)));
rxp0_1=(((px*r20))+(((-1.0)*r00*x65)));
rxp0_2=(((py*r00))+(((-1.0)*r10*x66)));
rxp1_0=((((-1.0)*r21*x64))+((pz*r11)));
rxp1_1=(((px*r21))+(((-1.0)*r01*x65)));
rxp1_2=((((-1.0)*r11*x66))+((py*r01)));
rxp2_0=((((-1.0)*r22*x64))+((pz*r12)));
rxp2_1=((((-1.0)*r02*x65))+((px*r22)));
rxp2_2=(((py*r02))+(((-1.0)*r12*x66)));
{
IkReal j9array[2], cj9array[2], sj9array[2];
bool j9valid[2]={false};
_nj9 = 2;
if( (((-1.18441410190393)+(((2.9867963734811)*pp)))) < -1-IKFAST_SINCOS_THRESH || (((-1.18441410190393)+(((2.9867963734811)*pp)))) > 1+IKFAST_SINCOS_THRESH )
    continue;
IkReal x67=IKasin(((-1.18441410190393)+(((2.9867963734811)*pp))));
j9array[0]=((-1.34027003705633)+(((1.0)*x67)));
sj9array[0]=IKsin(j9array[0]);
cj9array[0]=IKcos(j9array[0]);
j9array[1]=((1.80132261653346)+(((-1.0)*x67)));
sj9array[1]=IKsin(j9array[1]);
cj9array[1]=IKcos(j9array[1]);
if( j9array[0] > IKPI )
{
    j9array[0]-=IK2PI;
}
else if( j9array[0] < -IKPI )
{    j9array[0]+=IK2PI;
}
j9valid[0] = true;
if( j9array[1] > IKPI )
{
    j9array[1]-=IK2PI;
}
else if( j9array[1] < -IKPI )
{    j9array[1]+=IK2PI;
}
j9valid[1] = true;
for(int ij9 = 0; ij9 < 2; ++ij9)
{
if( !j9valid[ij9] )
{
    continue;
}
_ij9[0] = ij9; _ij9[1] = -1;
for(int iij9 = ij9+1; iij9 < 2; ++iij9)
{
if( j9valid[iij9] && IKabs(cj9array[ij9]-cj9array[iij9]) < IKFAST_SOLUTION_THRESH && IKabs(sj9array[ij9]-sj9array[iij9]) < IKFAST_SOLUTION_THRESH )
{
    j9valid[iij9]=false; _ij9[1] = iij9; break;
}
}
j9 = j9array[ij9]; cj9 = cj9array[ij9]; sj9 = sj9array[ij9];

{
IkReal j4eval[2];
j4eval[0]=((IKabs(py))+(IKabs(px)));
j4eval[1]=((py*py)+(px*px));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x68=cj8*cj8;
IkReal x69=sj9*sj9;
IkReal x70=((13.3333333333333)*sj9);
IkReal x71=(cj9*x70);
IkReal x72=cj9*cj9;
IkReal x73=((3.0)*cj8);
j6eval[0]=((149.382716049383)+(((-2.0)*cj9*x68))+((x68*x72))+(((44.4444444444444)*x68*x69))+(((44.4444444444444)*x72))+x68+x69+(((24.4444444444444)*sj9))+(((162.962962962963)*cj9))+x71+(((-1.0)*x68*x71))+((x68*x70)));
j6eval[1]=((((66.6666666666667)*(IKabs(((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)))))))+(IKabs(((((-1.0)*cj9*x73))+x73+(((20.0)*cj8*sj9))))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
IkReal x74=cj8*cj8*cj8*cj8;
IkReal x75=py*py*py*py;
IkReal x76=sj8*sj8*sj8*sj8;
IkReal x77=px*px;
IkReal x78=py*py;
IkReal x79=(x77*x78);
IkReal x80=sj8*sj8;
IkReal x81=((2.0)*x80);
IkReal x82=cj8*cj8;
IkReal x83=(px*py);
j4eval[0]=(((x75*x76))+((x74*x75))+((x76*x79))+((x74*x79))+((x75*x81*x82))+((x77*x78*x81*x82)));
j4eval[1]=((IKabs((((x80*x83))+((x82*x83)))))+(IKabs((((x78*x82))+((x78*x80))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
continue; // no branches [j4, j6]

} else
{
{
IkReal j4array[2], cj4array[2], sj4array[2];
bool j4valid[2]={false};
_nj4 = 2;
IkReal x84=cj8*cj8;
IkReal x85=py*py;
IkReal x86=sj8*sj8;
IkReal x87=(((x85*x86))+((x84*x85)));
IkReal x88=((1.0)*px*py);
IkReal x89=((((-1.0)*x86*x88))+(((-1.0)*x84*x88)));
CheckValue<IkReal> x93 = IKatan2WithCheck(IkReal(x87),x89,IKFAST_ATAN2_MAGTHRESH);
if(!x93.valid){
continue;
}
IkReal x90=((-1.0)*(x93.value));
IkReal x91=((0.045)*py*sj8);
if((((x87*x87)+(x89*x89))) < -0.00001)
continue;
CheckValue<IkReal> x94=IKPowWithIntegerCheck(IKabs(IKsqrt(((x87*x87)+(x89*x89)))),-1);
if(!x94.valid){
continue;
}
if( (((x94.value)*((((cj9*x91))+(((-1.0)*x91))+(((-1.0)*(0.3)*py*sj8*sj9)))))) < -1-IKFAST_SINCOS_THRESH || (((x94.value)*((((cj9*x91))+(((-1.0)*x91))+(((-1.0)*(0.3)*py*sj8*sj9)))))) > 1+IKFAST_SINCOS_THRESH )
    continue;
IkReal x92=IKasin(((x94.value)*((((cj9*x91))+(((-1.0)*x91))+(((-1.0)*(0.3)*py*sj8*sj9))))));
j4array[0]=(x90+(((-1.0)*x92)));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=((3.14159265358979)+x92+x90);
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
for(int ij4 = 0; ij4 < 2; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 2; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[2];
IkReal x95=((0.045)*sj8);
IkReal x96=(cj9*x95);
IkReal x97=((0.3)*sj8*sj9);
IkReal x98=IKcos(j4);
IkReal x99=cj8*cj8;
IkReal x100=(px*py);
IkReal x101=sj8*sj8;
IkReal x102=IKsin(j4);
IkReal x103=((1.0)*(px*px));
evalcond[0]=((((-1.0)*px*x95))+((px*x96))+((x98*((((x100*x101))+((x100*x99))))))+(((-1.0)*px*x97))+((x102*(((((-1.0)*x101*x103))+(((-1.0)*x103*x99)))))));
evalcond[1]=(x95+x97+((px*x102))+(((-1.0)*x96))+(((-1.0)*py*x98)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
IkReal j6eval[2];
IkReal x104=(cj4*px);
IkReal x105=(cj8*pz);
IkReal x106=(py*sj4);
IkReal x107=(cj4*cj9*px);
IkReal x108=(cj4*px*sj9);
IkReal x109=(cj8*pz*sj9);
IkReal x110=(cj9*py*sj4);
IkReal x111=(py*sj4*sj9);
IkReal x112=((0.045)*x105);
j6eval[0]=((((-1.0)*x111))+(((-1.0)*x108))+(((-12.2222222222222)*x106))+(((-6.66666666666667)*x109))+((cj9*x105))+(((-12.2222222222222)*x104))+(((-1.0)*x105))+(((-6.66666666666667)*x110))+(((-6.66666666666667)*x107)));
j6eval[1]=IKsign(((((-1.0)*x112))+(((-0.045)*x108))+(((-0.55)*x106))+(((-0.55)*x104))+((cj9*x112))+(((-0.3)*x107))+(((-0.045)*x111))+(((-0.3)*x110))+(((-0.3)*x109))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x113=(sj8*(py*py));
IkReal x114=cj4*cj4;
IkReal x115=(((sj8*x114*(px*px)))+((sj8*(pz*pz)))+(((-1.0)*x113*x114))+(((2.0)*cj4*px*py*sj4*sj8))+x113);
j6eval[0]=x115;
j6eval[1]=IKsign(x115);
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x116=(pz*sj8);
IkReal x117=(cj9*pz*sj8);
IkReal x118=(pz*sj8*sj9);
IkReal x119=(cj8*sj8);
IkReal x120=(cj4*px*x119);
IkReal x121=(py*sj4*x119);
IkReal x122=((1.0)*cj9);
IkReal x123=(cj4*cj8*px*sj8*sj9);
IkReal x124=(cj8*py*sj4*sj8*sj9);
IkReal x125=((0.045)*x120);
IkReal x126=((0.045)*x121);
j6eval[0]=((((6.66666666666667)*x123))+(((-1.0)*x121*x122))+x120+x121+(((-12.2222222222222)*x116))+(((-1.0)*x120*x122))+(((6.66666666666667)*x124))+(((-6.66666666666667)*x117))+(((-1.0)*x118)));
j6eval[1]=IKsign(((((-1.0)*cj9*x126))+x125+x126+(((-0.55)*x116))+(((0.3)*x123))+(((0.3)*x124))+(((-0.045)*x118))+(((-0.3)*x117))+(((-1.0)*cj9*x125))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
IkReal x127=(((px*sj4))+(((-1.0)*(1.0)*cj4*py)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j8))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=x127;
evalcond[3]=x127;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x128=(cj9*pz);
IkReal x129=(cj4*px);
IkReal x130=(pp*pz);
IkReal x131=(py*sj4);
IkReal x132=(cj4*cj9*px);
IkReal x133=(cj4*pp*px);
IkReal x134=(cj9*py*sj4);
IkReal x135=(pp*py*sj4);
j6eval[0]=(x128+(((5.4333061668025)*x130))+(((13.9482024812098)*x129))+(((12.2222222222222)*x134))+(((2.92556370551481)*pz))+(((12.2222222222222)*x132))+(((-36.2220411120167)*x135))+(((13.9482024812098)*x131))+(((-36.2220411120167)*x133)));
j6eval[1]=IKsign(((((1.32323529411765)*x132))+(((0.316735294117647)*pz))+(((0.108264705882353)*x128))+(((-3.92156862745098)*x135))+(((1.32323529411765)*x134))+(((1.51009803921569)*x131))+(((1.51009803921569)*x129))+(((0.588235294117647)*x130))+(((-3.92156862745098)*x133))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x136=(cj4*px);
IkReal x137=(py*sj4);
IkReal x138=(cj9*pz);
IkReal x139=(pz*sj9);
IkReal x140=((1.0)*cj9);
IkReal x141=(cj4*px*sj9);
IkReal x142=(py*sj4*sj9);
IkReal x143=((0.045)*x136);
IkReal x144=((0.045)*x137);
j6eval[0]=(x137+x136+(((-1.0)*(12.2222222222222)*pz))+(((6.66666666666667)*x141))+(((-1.0)*x136*x140))+(((6.66666666666667)*x142))+(((-6.66666666666667)*x138))+(((-1.0)*x139))+(((-1.0)*x137*x140)));
j6eval[1]=IKsign(((((-0.3)*x138))+(((-1.0)*(0.55)*pz))+(((0.3)*x142))+x143+x144+(((0.3)*x141))+(((-0.045)*x139))+(((-1.0)*cj9*x143))+(((-1.0)*cj9*x144))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x145=(cj4*px);
IkReal x146=(cj9*pz);
IkReal x147=(pp*pz);
IkReal x148=(py*sj4);
IkReal x149=(cj4*cj9*px);
IkReal x150=(cj4*pp*px);
IkReal x151=(cj9*py*sj4);
IkReal x152=(pp*py*sj4);
j6eval[0]=((((-1.0)*x151))+(((-5.4333061668025)*x150))+(((-1.0)*x149))+(((-5.4333061668025)*x152))+(((12.2222222222222)*x146))+(((13.9482024812098)*pz))+(((-2.92556370551481)*x148))+(((-2.92556370551481)*x145))+(((-36.2220411120167)*x147)));
j6eval[1]=IKsign(((((-0.108264705882353)*x151))+(((1.51009803921569)*pz))+(((-0.588235294117647)*x152))+(((-0.316735294117647)*x148))+(((-0.588235294117647)*x150))+(((-0.108264705882353)*x149))+(((1.32323529411765)*x146))+(((-3.92156862745098)*x147))+(((-0.316735294117647)*x145))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[1];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j9), 6.28318530717959)))))+(IKabs(pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x153=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x153);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x153);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x153);
rxp2_1=(px*r22);
j6eval[0]=(((py*sj4))+((cj4*px)));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[1];
IkReal x154=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x154);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x154);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x154);
rxp2_1=(px*r22);
j6eval[0]=((-1.0)+(((-1.0)*(1.3840830449827)*(px*px)))+(((-1.0)*(1.3840830449827)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x155=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x155);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x155);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x155);
rxp2_1=(px*r22);
IkReal x156=(cj4*px);
IkReal x157=(py*sj4);
j6eval[0]=(x156+x157);
j6eval[1]=((((-1.0)*(1.3840830449827)*cj4*(px*px*px)))+(((-1.0)*x157))+(((-1.3840830449827)*x157*(px*px)))+(((-1.3840830449827)*x156*(py*py)))+(((-1.0)*x156))+(((-1.0)*(1.3840830449827)*sj4*(py*py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=-0.2125;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
j6array[0]=2.9927027059803;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=6.13429535957009;
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j4), 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x606=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x606);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x606);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x606);
rxp2_1=0;
px=0;
j4=0;
sj4=0;
cj4=1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x607=py*py;
CheckValue<IkReal> x609 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x607)))),((-2.83333333333333)+(((3.92156862745098)*x607))),IKFAST_ATAN2_MAGTHRESH);
if(!x609.valid){
continue;
}
IkReal x608=((-1.0)*(x609.value));
j6array[0]=x608;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x608);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(j4, 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x610=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x610);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x610);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x610);
rxp2_1=0;
px=0;
j4=3.14159265358979;
sj4=0;
cj4=-1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x611=py*py;
CheckValue<IkReal> x613 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x611)))),((-2.83333333333333)+(((3.92156862745098)*x611))),IKFAST_ATAN2_MAGTHRESH);
if(!x613.valid){
continue;
}
IkReal x612=((-1.0)*(x613.value));
j6array[0]=x612;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x612);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((1.5707963267949)+j4), 6.28318530717959)))))+(IKabs(py)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x614=((1.0)*px);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=1.5707963267949;
sj4=1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x614);
rxp1_2=((-1.0)*r11*x614);
rxp2_2=((-1.0)*r12*x614);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x615=px*px;
CheckValue<IkReal> x617 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x615)))),((-2.83333333333333)+(((3.92156862745098)*x615))),IKFAST_ATAN2_MAGTHRESH);
if(!x617.valid){
continue;
}
IkReal x616=((-1.0)*(x617.value));
j6array[0]=x616;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x616);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(((-3.14159265358979)+(IKfmod(((4.71238898038469)+j4), 6.28318530717959))))));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x618=((1.0)*px);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=-1.5707963267949;
sj4=-1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x618);
rxp1_2=((-1.0)*r11*x618);
rxp2_2=((-1.0)*r12*x618);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x619=px*px;
CheckValue<IkReal> x621 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x619)))),((-2.83333333333333)+(((3.92156862745098)*x619))),IKFAST_ATAN2_MAGTHRESH);
if(!x621.valid){
continue;
}
IkReal x620=((-1.0)*(x621.value));
j6array[0]=x620;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x620);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x622=(cj4*px);
IkReal x623=(py*sj4);
IkReal x624=px*px;
IkReal x625=py*py;
CheckValue<IkReal> x626=IKPowWithIntegerCheck(((((20.0)*x622))+(((20.0)*x623))),-1);
if(!x626.valid){
continue;
}
CheckValue<IkReal> x627=IKPowWithIntegerCheck(((((-1.0)*(11.7647058823529)*cj4*(px*px*px)))+(((-8.5)*x623))+(((-11.7647058823529)*x623*x624))+(((-11.7647058823529)*x622*x625))+(((-8.5)*x622))+(((-1.0)*(11.7647058823529)*sj4*(py*py*py)))),-1);
if(!x627.valid){
continue;
}
if( IKabs(((17.0)*(x626.value))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x627.value)*(((48.1666666666667)+(((-66.6666666666667)*x625))+(((-66.6666666666667)*x624)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((17.0)*(x626.value)))+IKsqr(((x627.value)*(((48.1666666666667)+(((-66.6666666666667)*x625))+(((-66.6666666666667)*x624))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((17.0)*(x626.value)), ((x627.value)*(((48.1666666666667)+(((-66.6666666666667)*x625))+(((-66.6666666666667)*x624))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x628=IKsin(j6);
IkReal x629=(cj4*px);
IkReal x630=(x628*x629);
IkReal x631=(py*sj4);
IkReal x632=(x628*x631);
IkReal x633=((1.0)*x629);
IkReal x634=((1.0)*x631);
IkReal x635=IKcos(j6);
IkReal x636=px*px;
IkReal x637=((3.92156862745098)*x628);
IkReal x638=((0.588235294117647)*x635);
IkReal x639=py*py;
IkReal x640=((0.09)*x635);
evalcond[0]=((-0.85)+x630+x632);
evalcond[1]=((((-1.0)*x634))+(((0.85)*x628))+(((-1.0)*x633)));
evalcond[2]=((((-1.0)*x634*x635))+(((-1.0)*x633*x635)));
evalcond[3]=(((x637*x639))+((x636*x637))+(((-1.0)*x638*x639))+(((-0.425)*x635))+(((-2.83333333333333)*x628))+(((-1.0)*x636*x638)));
evalcond[4]=((-0.2125)+(((-1.0)*x639))+((x631*x640))+(((-1.0)*x636))+((x629*x640))+(((1.1)*x632))+(((1.1)*x630)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x641=(cj4*px);
IkReal x642=(py*sj4);
IkReal x643=px*px;
IkReal x644=py*py;
CheckValue<IkReal> x645=IKPowWithIntegerCheck(((-7.225)+(((-10.0)*x643))+(((-10.0)*x644))),-1);
if(!x645.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x642))+(((1.17647058823529)*x641)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x645.value)*(((((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x641*x644))+(((-78.4313725490196)*x642*x643))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((56.6666666666667)*x642))+(((56.6666666666667)*x641)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x642))+(((1.17647058823529)*x641))))+IKsqr(((x645.value)*(((((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x641*x644))+(((-78.4313725490196)*x642*x643))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((56.6666666666667)*x642))+(((56.6666666666667)*x641))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x642))+(((1.17647058823529)*x641))), ((x645.value)*(((((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x641*x644))+(((-78.4313725490196)*x642*x643))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((56.6666666666667)*x642))+(((56.6666666666667)*x641))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x646=IKsin(j6);
IkReal x647=(cj4*px);
IkReal x648=(x646*x647);
IkReal x649=(py*sj4);
IkReal x650=(x646*x649);
IkReal x651=((1.0)*x647);
IkReal x652=((1.0)*x649);
IkReal x653=IKcos(j6);
IkReal x654=px*px;
IkReal x655=((3.92156862745098)*x646);
IkReal x656=((0.588235294117647)*x653);
IkReal x657=py*py;
IkReal x658=((0.09)*x653);
evalcond[0]=((-0.85)+x650+x648);
evalcond[1]=((((-1.0)*x652))+(((-1.0)*x651))+(((0.85)*x646)));
evalcond[2]=((((-1.0)*x652*x653))+(((-1.0)*x651*x653)));
evalcond[3]=(((x654*x655))+((x655*x657))+(((-1.0)*x654*x656))+(((-0.425)*x653))+(((-1.0)*x656*x657))+(((-2.83333333333333)*x646)));
evalcond[4]=((-0.2125)+(((1.1)*x648))+(((1.1)*x650))+((x649*x658))+(((-1.0)*x657))+((x647*x658))+(((-1.0)*x654)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x659=(cj4*px);
IkReal x660=(py*sj4);
IkReal x661=px*px;
IkReal x662=py*py;
IkReal x663=((1.29411764705882)*(cj4*cj4));
CheckValue<IkReal> x664=IKPowWithIntegerCheck(((((0.09)*x659))+(((0.09)*x660))),-1);
if(!x664.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x660))+(((1.17647058823529)*x659)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x664.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x660))+(((-0.294117647058824)*x662))+(((-1.0)*x661*x663))+((x662*x663))+x661)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x660))+(((1.17647058823529)*x659))))+IKsqr(((x664.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x660))+(((-0.294117647058824)*x662))+(((-1.0)*x661*x663))+((x662*x663))+x661))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x660))+(((1.17647058823529)*x659))), ((x664.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x660))+(((-0.294117647058824)*x662))+(((-1.0)*x661*x663))+((x662*x663))+x661))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x665=IKsin(j6);
IkReal x666=(cj4*px);
IkReal x667=(x665*x666);
IkReal x668=(py*sj4);
IkReal x669=(x665*x668);
IkReal x670=((1.0)*x666);
IkReal x671=((1.0)*x668);
IkReal x672=IKcos(j6);
IkReal x673=px*px;
IkReal x674=((3.92156862745098)*x665);
IkReal x675=((0.588235294117647)*x672);
IkReal x676=py*py;
IkReal x677=((0.09)*x672);
evalcond[0]=((-0.85)+x667+x669);
evalcond[1]=((((0.85)*x665))+(((-1.0)*x670))+(((-1.0)*x671)));
evalcond[2]=((((-1.0)*x671*x672))+(((-1.0)*x670*x672)));
evalcond[3]=((((-2.83333333333333)*x665))+(((-0.425)*x672))+((x673*x674))+(((-1.0)*x675*x676))+((x674*x676))+(((-1.0)*x673*x675)));
evalcond[4]=((-0.2125)+((x668*x677))+(((-1.0)*x676))+((x666*x677))+(((1.1)*x667))+(((-1.0)*x673))+(((1.1)*x669)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x678=(cj4*px);
IkReal x679=(py*sj4);
IkReal x680=((0.108264705882353)*cj9);
IkReal x681=((0.588235294117647)*pp);
IkReal x682=(cj9*pp);
IkReal x683=(cj9*sj9);
IkReal x684=(pp*sj9);
IkReal x685=cj9*cj9;
IkReal x686=((1.0)*pz);
CheckValue<IkReal> x687=IKPowWithIntegerCheck(IKsign(((((-1.0)*x678*x681))+(((-1.0)*x679*x680))+(((1.51009803921569)*pz))+(((1.32323529411765)*cj9*pz))+(((-1.0)*(3.92156862745098)*pp*pz))+(((-1.0)*x678*x680))+(((-0.316735294117647)*x679))+(((-0.316735294117647)*x678))+(((-1.0)*x679*x681)))),-1);
if(!x687.valid){
continue;
}
CheckValue<IkReal> x688 = IKatan2WithCheck(IkReal(((-0.174204411764706)+(((-0.0264705882352941)*x684))+(((-0.00487191176470588)*x683))+(pz*pz)+(((-0.0324794117647059)*x685))+(((-1.0)*(0.154566176470588)*cj9))+(((-0.176470588235294)*x682))+(((-1.0)*(0.323529411764706)*pp))+(((-1.0)*(0.0142530882352941)*sj9)))),((0.830553921568627)+(((-1.17647058823529)*x682))+(((0.396970588235294)*x685))+(((1.18080882352941)*cj9))+(((0.0595455882352941)*x683))+(((-1.0)*(2.15686274509804)*pp))+(((-1.0)*x678*x686))+(((-1.0)*x679*x686))+(((-0.176470588235294)*x684))+(((0.0679544117647059)*sj9))),IKFAST_ATAN2_MAGTHRESH);
if(!x688.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x687.value)))+(x688.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x689=((0.3)*cj9);
IkReal x690=((0.045)*sj9);
IkReal x691=IKcos(j6);
IkReal x692=(pz*x691);
IkReal x693=IKsin(j6);
IkReal x694=(cj4*px);
IkReal x695=(x693*x694);
IkReal x696=(py*sj4);
IkReal x697=(x693*x696);
IkReal x698=((0.045)*cj9);
IkReal x699=((0.3)*sj9);
IkReal x700=(pz*x693);
IkReal x701=((1.0)*x694);
IkReal x702=((1.0)*x696);
IkReal x703=((0.09)*x691);
evalcond[0]=((-0.55)+(((-1.0)*x689))+(((-1.0)*x690))+x692+x695+x697);
evalcond[1]=((0.045)+x700+(((-1.0)*x691*x701))+(((-1.0)*x698))+x699+(((-1.0)*x691*x702)));
evalcond[2]=((((-1.51009803921569)*x693))+(((-0.588235294117647)*pp*x691))+(((3.92156862745098)*pp*x693))+pz+(((-1.32323529411765)*cj9*x693))+(((-0.316735294117647)*x691))+(((-0.108264705882353)*cj9*x691)));
evalcond[3]=((((-1.0)*x691*x698))+(((-1.0)*x702))+((x690*x693))+((x691*x699))+(((0.55)*x693))+(((0.045)*x691))+((x689*x693))+(((-1.0)*x701)));
evalcond[4]=((-0.2125)+((x696*x703))+(((1.1)*x695))+(((-0.09)*x700))+(((1.1)*x692))+(((1.1)*x697))+((x694*x703))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x704=((0.045)*cj4*px);
IkReal x705=((0.045)*py*sj4);
IkReal x706=((0.3)*sj9);
IkReal x707=(cj4*px);
IkReal x708=(py*sj4);
IkReal x709=(cj9*sj9);
IkReal x710=cj9*cj9;
IkReal x711=((1.0)*pz);
IkReal x712=py*py;
IkReal x713=cj4*cj4;
CheckValue<IkReal> x714 = IKatan2WithCheck(IkReal(((0.03825)+(((0.087975)*x709))+(((-0.027)*x710))+(((-1.0)*x707*x711))+(((0.167025)*sj9))+(((-1.0)*(0.01125)*cj9))+(((-1.0)*x708*x711)))),((-0.304525)+(((-1.0)*(0.0495)*sj9))+(((-0.087975)*x710))+(((2.0)*cj4*px*x708))+x712+(((-1.0)*x712*x713))+(((-1.0)*(0.33)*cj9))+((x713*(px*px)))+(((-0.027)*x709))),IKFAST_ATAN2_MAGTHRESH);
if(!x714.valid){
continue;
}
CheckValue<IkReal> x715=IKPowWithIntegerCheck(IKsign(((((-1.0)*cj9*x704))+(((-1.0)*(0.55)*pz))+((x706*x708))+((x706*x707))+x705+x704+(((-1.0)*cj9*x705))+(((-1.0)*(0.045)*pz*sj9))+(((-1.0)*(0.3)*cj9*pz)))),-1);
if(!x715.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x714.value)+(((1.5707963267949)*(x715.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x716=((0.3)*cj9);
IkReal x717=((0.045)*sj9);
IkReal x718=IKcos(j6);
IkReal x719=(pz*x718);
IkReal x720=IKsin(j6);
IkReal x721=(cj4*px);
IkReal x722=(x720*x721);
IkReal x723=(py*sj4);
IkReal x724=(x720*x723);
IkReal x725=((0.045)*cj9);
IkReal x726=((0.3)*sj9);
IkReal x727=(pz*x720);
IkReal x728=((1.0)*x721);
IkReal x729=((1.0)*x723);
IkReal x730=((0.09)*x718);
evalcond[0]=((-0.55)+x719+(((-1.0)*x716))+(((-1.0)*x717))+x724+x722);
evalcond[1]=((0.045)+(((-1.0)*x718*x729))+(((-1.0)*x725))+(((-1.0)*x718*x728))+x727+x726);
evalcond[2]=((((3.92156862745098)*pp*x720))+(((-1.32323529411765)*cj9*x720))+(((-0.108264705882353)*cj9*x718))+(((-1.51009803921569)*x720))+pz+(((-0.588235294117647)*pp*x718))+(((-0.316735294117647)*x718)));
evalcond[3]=(((x717*x720))+(((-1.0)*x718*x725))+(((0.045)*x718))+((x716*x720))+(((-1.0)*x728))+(((0.55)*x720))+(((-1.0)*x729))+((x718*x726)));
evalcond[4]=((-0.2125)+(((1.1)*x722))+(((-0.09)*x727))+(((1.1)*x719))+((x723*x730))+(((1.1)*x724))+(((-1.0)*(1.0)*pp))+((x721*x730)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x731=(cj4*px);
IkReal x732=(py*sj4);
IkReal x733=((1.32323529411765)*cj9);
IkReal x734=((3.92156862745098)*pp);
IkReal x735=((0.0264705882352941)*pp);
IkReal x736=(cj9*sj9);
IkReal x737=((0.176470588235294)*pp);
IkReal x738=cj9*cj9;
CheckValue<IkReal> x739 = IKatan2WithCheck(IkReal(((-0.0142530882352941)+(((-0.0324794117647059)*x736))+(((0.00938117647058823)*cj9))+((pz*x732))+((pz*x731))+(((-1.0)*x735))+(((0.00487191176470588)*x738))+(((-1.0)*(0.0950205882352941)*sj9))+((cj9*x735))+(((-1.0)*sj9*x737)))),((0.0679544117647059)+(((-1.0)*x737))+((cj9*x737))+(pz*pz)+(((-1.0)*(0.00840882352941177)*cj9))+(((-0.0595455882352941)*x738))+(((0.453029411764706)*sj9))+(((0.396970588235294)*x736))+(((-1.0)*(1.17647058823529)*pp*sj9))),IKFAST_ATAN2_MAGTHRESH);
if(!x739.valid){
continue;
}
CheckValue<IkReal> x740=IKPowWithIntegerCheck(IKsign(((((0.316735294117647)*pz))+(((0.108264705882353)*cj9*pz))+(((0.588235294117647)*pp*pz))+((x732*x733))+(((-1.0)*x732*x734))+(((1.51009803921569)*x731))+(((1.51009803921569)*x732))+((x731*x733))+(((-1.0)*x731*x734)))),-1);
if(!x740.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x739.value)+(((1.5707963267949)*(x740.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x741=((0.3)*cj9);
IkReal x742=((0.045)*sj9);
IkReal x743=IKcos(j6);
IkReal x744=(pz*x743);
IkReal x745=IKsin(j6);
IkReal x746=(cj4*px);
IkReal x747=(x745*x746);
IkReal x748=(py*sj4);
IkReal x749=(x745*x748);
IkReal x750=((0.045)*cj9);
IkReal x751=((0.3)*sj9);
IkReal x752=(pz*x745);
IkReal x753=((1.0)*x746);
IkReal x754=((1.0)*x748);
IkReal x755=((0.09)*x743);
evalcond[0]=((-0.55)+(((-1.0)*x741))+x749+x744+x747+(((-1.0)*x742)));
evalcond[1]=((0.045)+(((-1.0)*x743*x753))+(((-1.0)*x743*x754))+x752+x751+(((-1.0)*x750)));
evalcond[2]=((((-0.316735294117647)*x743))+(((-1.32323529411765)*cj9*x745))+pz+(((-0.108264705882353)*cj9*x743))+(((-1.51009803921569)*x745))+(((-0.588235294117647)*pp*x743))+(((3.92156862745098)*pp*x745)));
evalcond[3]=((((-1.0)*x753))+(((0.55)*x745))+(((0.045)*x743))+((x741*x745))+((x743*x751))+(((-1.0)*x743*x750))+(((-1.0)*x754))+((x742*x745)));
evalcond[4]=((-0.2125)+(((1.1)*x749))+(((-0.09)*x752))+((x748*x755))+(((1.1)*x744))+(((-1.0)*(1.0)*pp))+((x746*x755))+(((1.1)*x747)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x756=(px*sj4);
IkReal x757=(cj4*py);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j8)))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=((((-1.0)*x757))+x756);
evalcond[3]=((((-1.0)*x756))+x757);
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x758=py*py;
IkReal x759=cj4*cj4;
IkReal x760=((pz*pz)+((x759*(px*px)))+(((2.0)*cj4*px*py*sj4))+x758+(((-1.0)*x758*x759)));
j6eval[0]=x760;
j6eval[1]=IKsign(x760);
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x761=(cj4*px);
IkReal x762=(cj9*pz);
IkReal x763=(py*sj4);
IkReal x764=(pz*sj9);
IkReal x765=(cj4*px*sj9);
IkReal x766=(py*sj4*sj9);
IkReal x767=((0.045)*x761);
IkReal x768=((0.045)*x763);
j6eval[0]=(((cj9*x761))+(((-1.0)*(12.2222222222222)*pz))+(((-1.0)*x764))+(((-6.66666666666667)*x765))+(((-1.0)*x763))+(((-6.66666666666667)*x766))+((cj9*x763))+(((-6.66666666666667)*x762))+(((-1.0)*x761)));
j6eval[1]=IKsign(((((-0.3)*x766))+(((-1.0)*x767))+(((-1.0)*(0.55)*pz))+((cj9*x768))+(((-0.3)*x765))+(((-1.0)*x768))+(((-0.3)*x762))+(((-0.045)*x764))+((cj9*x767))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x769=(cj4*px);
IkReal x770=(cj9*pz);
IkReal x771=(pp*pz);
IkReal x772=(py*sj4);
IkReal x773=(cj4*cj9*px);
IkReal x774=(cj4*pp*px);
IkReal x775=(cj9*py*sj4);
IkReal x776=(pp*py*sj4);
j6eval[0]=((((-1.0)*x775))+(((-1.0)*(13.9482024812098)*pz))+(((-2.92556370551481)*x772))+(((-12.2222222222222)*x770))+(((-5.4333061668025)*x776))+(((-1.0)*x773))+(((-5.4333061668025)*x774))+(((-2.92556370551481)*x769))+(((36.2220411120167)*x771)));
j6eval[1]=IKsign(((((-0.588235294117647)*x776))+(((-0.108264705882353)*x773))+(((3.92156862745098)*x771))+(((-0.316735294117647)*x772))+(((-1.0)*(1.51009803921569)*pz))+(((-0.316735294117647)*x769))+(((-1.32323529411765)*x770))+(((-0.588235294117647)*x774))+(((-0.108264705882353)*x775))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[1];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j9), 6.28318530717959)))))+(IKabs(pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x777=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x777);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x777);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x777);
rxp2_1=(px*r22);
j6eval[0]=((((-1.0)*(1.0)*cj4*px))+(((-1.0)*(1.0)*py*sj4)));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[1];
IkReal x778=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x778);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x778);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x778);
rxp2_1=(px*r22);
j6eval[0]=((-1.0)+(((-1.0)*(1.3840830449827)*(px*px)))+(((-1.0)*(1.3840830449827)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x779=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x779);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x779);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x779);
rxp2_1=(px*r22);
IkReal x780=(cj4*px);
IkReal x781=(py*sj4);
j6eval[0]=(x780+x781);
j6eval[1]=((((-1.0)*(1.3840830449827)*cj4*(px*px*px)))+(((-1.0)*x780))+(((-1.3840830449827)*x780*(py*py)))+(((-1.0)*x781))+(((-1.0)*(1.3840830449827)*sj4*(py*py*py)))+(((-1.3840830449827)*x781*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=-0.2125;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
j6array[0]=0.148889947609497;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=3.29048260119929;
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j4), 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x782=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x782);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x782);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x782);
rxp2_1=0;
px=0;
j4=0;
sj4=0;
cj4=1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x783=py*py;
CheckValue<IkReal> x785 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x783)))),((2.83333333333333)+(((-3.92156862745098)*x783))),IKFAST_ATAN2_MAGTHRESH);
if(!x785.valid){
continue;
}
IkReal x784=((-1.0)*(x785.value));
j6array[0]=x784;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x784);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(j4, 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x786=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x786);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x786);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x786);
rxp2_1=0;
px=0;
j4=3.14159265358979;
sj4=0;
cj4=-1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x787=py*py;
CheckValue<IkReal> x789 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x787)))),((2.83333333333333)+(((-3.92156862745098)*x787))),IKFAST_ATAN2_MAGTHRESH);
if(!x789.valid){
continue;
}
IkReal x788=((-1.0)*(x789.value));
j6array[0]=x788;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x788);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((1.5707963267949)+j4), 6.28318530717959)))))+(IKabs(py)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x790=((1.0)*px);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=1.5707963267949;
sj4=1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x790);
rxp1_2=((-1.0)*r11*x790);
rxp2_2=((-1.0)*r12*x790);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x791=px*px;
CheckValue<IkReal> x793 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x791)))),((2.83333333333333)+(((-3.92156862745098)*x791))),IKFAST_ATAN2_MAGTHRESH);
if(!x793.valid){
continue;
}
IkReal x792=((-1.0)*(x793.value));
j6array[0]=x792;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x792);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(((-3.14159265358979)+(IKfmod(((4.71238898038469)+j4), 6.28318530717959))))));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x794=((1.0)*px);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=-1.5707963267949;
sj4=-1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x794);
rxp1_2=((-1.0)*r11*x794);
rxp2_2=((-1.0)*r12*x794);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x795=px*px;
CheckValue<IkReal> x797 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x795)))),((2.83333333333333)+(((-3.92156862745098)*x795))),IKFAST_ATAN2_MAGTHRESH);
if(!x797.valid){
continue;
}
IkReal x796=((-1.0)*(x797.value));
j6array[0]=x796;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x796);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x798=(cj4*px);
IkReal x799=(py*sj4);
IkReal x800=px*px;
IkReal x801=py*py;
CheckValue<IkReal> x802=IKPowWithIntegerCheck(((((20.0)*x799))+(((20.0)*x798))),-1);
if(!x802.valid){
continue;
}
CheckValue<IkReal> x803=IKPowWithIntegerCheck(((((-8.5)*x798))+(((-1.0)*(11.7647058823529)*cj4*(px*px*px)))+(((-11.7647058823529)*x798*x801))+(((-11.7647058823529)*x799*x800))+(((-8.5)*x799))+(((-1.0)*(11.7647058823529)*sj4*(py*py*py)))),-1);
if(!x803.valid){
continue;
}
if( IKabs(((17.0)*(x802.value))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x803.value)*(((-48.1666666666667)+(((66.6666666666667)*x800))+(((66.6666666666667)*x801)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((17.0)*(x802.value)))+IKsqr(((x803.value)*(((-48.1666666666667)+(((66.6666666666667)*x800))+(((66.6666666666667)*x801))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((17.0)*(x802.value)), ((x803.value)*(((-48.1666666666667)+(((66.6666666666667)*x800))+(((66.6666666666667)*x801))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x804=IKcos(j6);
IkReal x805=(cj4*px);
IkReal x806=(x804*x805);
IkReal x807=(py*sj4);
IkReal x808=(x804*x807);
IkReal x809=IKsin(j6);
IkReal x810=(x805*x809);
IkReal x811=(x807*x809);
IkReal x812=px*px;
IkReal x813=((3.92156862745098)*x809);
IkReal x814=((0.588235294117647)*x804);
IkReal x815=py*py;
evalcond[0]=(x806+x808);
evalcond[1]=((-0.85)+x811+x810);
evalcond[2]=((((-1.0)*x807))+(((0.85)*x809))+(((-1.0)*x805)));
evalcond[3]=((((-1.0)*x812*x814))+(((-1.0)*x814*x815))+(((-1.0)*x812*x813))+(((2.83333333333333)*x809))+(((-0.425)*x804))+(((-1.0)*x813*x815)));
evalcond[4]=((-0.2125)+(((1.1)*x810))+(((-0.09)*x808))+(((-1.0)*x815))+(((1.1)*x811))+(((-1.0)*x812))+(((-0.09)*x806)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x816=(cj4*px);
IkReal x817=(py*sj4);
IkReal x818=px*px;
IkReal x819=py*py;
CheckValue<IkReal> x820=IKPowWithIntegerCheck(((-7.225)+(((-10.0)*x818))+(((-10.0)*x819))),-1);
if(!x820.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x816))+(((1.17647058823529)*x817)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x820.value)*(((((78.4313725490196)*x817*x818))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((-56.6666666666667)*x816))+(((-56.6666666666667)*x817))+(((78.4313725490196)*x816*x819)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x816))+(((1.17647058823529)*x817))))+IKsqr(((x820.value)*(((((78.4313725490196)*x817*x818))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((-56.6666666666667)*x816))+(((-56.6666666666667)*x817))+(((78.4313725490196)*x816*x819))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x816))+(((1.17647058823529)*x817))), ((x820.value)*(((((78.4313725490196)*x817*x818))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((-56.6666666666667)*x816))+(((-56.6666666666667)*x817))+(((78.4313725490196)*x816*x819))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x821=IKcos(j6);
IkReal x822=(cj4*px);
IkReal x823=(x821*x822);
IkReal x824=(py*sj4);
IkReal x825=(x821*x824);
IkReal x826=IKsin(j6);
IkReal x827=(x822*x826);
IkReal x828=(x824*x826);
IkReal x829=px*px;
IkReal x830=((3.92156862745098)*x826);
IkReal x831=((0.588235294117647)*x821);
IkReal x832=py*py;
evalcond[0]=(x825+x823);
evalcond[1]=((-0.85)+x827+x828);
evalcond[2]=((((0.85)*x826))+(((-1.0)*x822))+(((-1.0)*x824)));
evalcond[3]=((((-1.0)*x830*x832))+(((2.83333333333333)*x826))+(((-1.0)*x829*x831))+(((-1.0)*x831*x832))+(((-1.0)*x829*x830))+(((-0.425)*x821)));
evalcond[4]=((-0.2125)+(((1.1)*x828))+(((-0.09)*x825))+(((1.1)*x827))+(((-0.09)*x823))+(((-1.0)*x832))+(((-1.0)*x829)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x833=(cj4*px);
IkReal x834=(py*sj4);
IkReal x835=px*px;
IkReal x836=py*py;
IkReal x837=((1.29411764705882)*(cj4*cj4));
CheckValue<IkReal> x838=IKPowWithIntegerCheck(((((-0.09)*x834))+(((-0.09)*x833))),-1);
if(!x838.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x833))+(((1.17647058823529)*x834)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x838.value)*(((0.2125)+(((-0.294117647058824)*x836))+x835+(((-2.58823529411765)*cj4*px*x834))+((x836*x837))+(((-1.0)*x835*x837)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x833))+(((1.17647058823529)*x834))))+IKsqr(((x838.value)*(((0.2125)+(((-0.294117647058824)*x836))+x835+(((-2.58823529411765)*cj4*px*x834))+((x836*x837))+(((-1.0)*x835*x837))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x833))+(((1.17647058823529)*x834))), ((x838.value)*(((0.2125)+(((-0.294117647058824)*x836))+x835+(((-2.58823529411765)*cj4*px*x834))+((x836*x837))+(((-1.0)*x835*x837))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x839=IKcos(j6);
IkReal x840=(cj4*px);
IkReal x841=(x839*x840);
IkReal x842=(py*sj4);
IkReal x843=(x839*x842);
IkReal x844=IKsin(j6);
IkReal x845=(x840*x844);
IkReal x846=(x842*x844);
IkReal x847=px*px;
IkReal x848=((3.92156862745098)*x844);
IkReal x849=((0.588235294117647)*x839);
IkReal x850=py*py;
evalcond[0]=(x843+x841);
evalcond[1]=((-0.85)+x846+x845);
evalcond[2]=((((0.85)*x844))+(((-1.0)*x842))+(((-1.0)*x840)));
evalcond[3]=((((-1.0)*x847*x849))+(((-1.0)*x847*x848))+(((-0.425)*x839))+(((-1.0)*x848*x850))+(((-1.0)*x849*x850))+(((2.83333333333333)*x844)));
evalcond[4]=((-0.2125)+(((-1.0)*x850))+(((-0.09)*x843))+(((1.1)*x846))+(((1.1)*x845))+(((-1.0)*x847))+(((-0.09)*x841)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x851=(cj4*px);
IkReal x852=(py*sj4);
IkReal x853=((0.108264705882353)*cj9);
IkReal x854=((0.588235294117647)*pp);
IkReal x855=(cj9*pp);
IkReal x856=(cj9*sj9);
IkReal x857=(pp*sj9);
IkReal x858=cj9*cj9;
IkReal x859=((1.0)*pz);
CheckValue<IkReal> x860 = IKatan2WithCheck(IkReal(((-0.174204411764706)+(pz*pz)+(((-0.176470588235294)*x855))+(((-1.0)*(0.154566176470588)*cj9))+(((-1.0)*(0.323529411764706)*pp))+(((-0.0264705882352941)*x857))+(((-0.00487191176470588)*x856))+(((-1.0)*(0.0142530882352941)*sj9))+(((-0.0324794117647059)*x858)))),((-0.830553921568627)+(((-0.396970588235294)*x858))+(((-1.0)*(0.0679544117647059)*sj9))+(((-1.0)*(1.18080882352941)*cj9))+(((0.176470588235294)*x857))+(((-1.0)*x852*x859))+(((2.15686274509804)*pp))+(((1.17647058823529)*x855))+(((-1.0)*x851*x859))+(((-0.0595455882352941)*x856))),IKFAST_ATAN2_MAGTHRESH);
if(!x860.valid){
continue;
}
CheckValue<IkReal> x861=IKPowWithIntegerCheck(IKsign(((((-1.0)*x851*x854))+(((-1.0)*(1.32323529411765)*cj9*pz))+(((3.92156862745098)*pp*pz))+(((-1.0)*(1.51009803921569)*pz))+(((-0.316735294117647)*x852))+(((-1.0)*x852*x853))+(((-0.316735294117647)*x851))+(((-1.0)*x851*x853))+(((-1.0)*x852*x854)))),-1);
if(!x861.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x860.value)+(((1.5707963267949)*(x861.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x862=((0.3)*cj9);
IkReal x863=((0.045)*sj9);
IkReal x864=IKcos(j6);
IkReal x865=(pz*x864);
IkReal x866=IKsin(j6);
IkReal x867=(cj4*px);
IkReal x868=(x866*x867);
IkReal x869=(py*sj4);
IkReal x870=(x866*x869);
IkReal x871=((0.045)*cj9);
IkReal x872=((0.3)*sj9);
IkReal x873=(pz*x866);
IkReal x874=(x864*x867);
IkReal x875=(x864*x869);
evalcond[0]=((-0.55)+(((-1.0)*x862))+x868+x865+x870+(((-1.0)*x863)));
evalcond[1]=((0.045)+(((-1.0)*x873))+x872+x874+x875+(((-1.0)*x871)));
evalcond[2]=((((1.51009803921569)*x866))+(((-0.316735294117647)*x864))+pz+(((-3.92156862745098)*pp*x866))+(((-0.108264705882353)*cj9*x864))+(((-0.588235294117647)*pp*x864))+(((1.32323529411765)*cj9*x866)));
evalcond[3]=((((0.55)*x866))+(((-1.0)*x869))+(((-0.045)*x864))+(((-1.0)*x867))+(((-1.0)*x864*x872))+((x862*x866))+((x864*x871))+((x863*x866)));
evalcond[4]=((-0.2125)+(((-0.09)*x875))+(((1.1)*x865))+(((0.09)*x873))+(((-0.09)*x874))+(((1.1)*x870))+(((1.1)*x868))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x876=((0.045)*cj4*px);
IkReal x877=((0.045)*py*sj4);
IkReal x878=((0.3)*sj9);
IkReal x879=(cj4*px);
IkReal x880=(py*sj4);
IkReal x881=(cj9*sj9);
IkReal x882=cj9*cj9;
IkReal x883=((1.0)*pz);
IkReal x884=py*py;
IkReal x885=cj4*cj4;
CheckValue<IkReal> x886 = IKatan2WithCheck(IkReal(((-0.03825)+(((0.027)*x882))+(((-0.087975)*x881))+(((-1.0)*(0.167025)*sj9))+(((-1.0)*x879*x883))+(((-1.0)*x880*x883))+(((0.01125)*cj9)))),((-0.304525)+(((-1.0)*x884*x885))+(((2.0)*cj4*px*x880))+(((-1.0)*(0.0495)*sj9))+(((-0.087975)*x882))+((x885*(px*px)))+x884+(((-1.0)*(0.33)*cj9))+(((-0.027)*x881))),IKFAST_ATAN2_MAGTHRESH);
if(!x886.valid){
continue;
}
CheckValue<IkReal> x887=IKPowWithIntegerCheck(IKsign(((((-1.0)*x878*x879))+(((-1.0)*(0.55)*pz))+(((-1.0)*x877))+(((-1.0)*x876))+(((-1.0)*x878*x880))+((cj9*x877))+((cj9*x876))+(((-1.0)*(0.045)*pz*sj9))+(((-1.0)*(0.3)*cj9*pz)))),-1);
if(!x887.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x886.value)+(((1.5707963267949)*(x887.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x888=((0.3)*cj9);
IkReal x889=((0.045)*sj9);
IkReal x890=IKcos(j6);
IkReal x891=(pz*x890);
IkReal x892=IKsin(j6);
IkReal x893=(cj4*px);
IkReal x894=(x892*x893);
IkReal x895=(py*sj4);
IkReal x896=(x892*x895);
IkReal x897=((0.045)*cj9);
IkReal x898=((0.3)*sj9);
IkReal x899=(pz*x892);
IkReal x900=(x890*x893);
IkReal x901=(x890*x895);
evalcond[0]=((-0.55)+(((-1.0)*x888))+x894+x896+x891+(((-1.0)*x889)));
evalcond[1]=((0.045)+(((-1.0)*x897))+x900+x901+x898+(((-1.0)*x899)));
evalcond[2]=((((-0.588235294117647)*pp*x890))+(((-0.316735294117647)*x890))+(((1.32323529411765)*cj9*x892))+pz+(((-0.108264705882353)*cj9*x890))+(((-3.92156862745098)*pp*x892))+(((1.51009803921569)*x892)));
evalcond[3]=((((-1.0)*x893))+((x890*x897))+(((0.55)*x892))+((x888*x892))+(((-1.0)*x890*x898))+((x889*x892))+(((-1.0)*x895))+(((-0.045)*x890)));
evalcond[4]=((-0.2125)+(((-0.09)*x900))+(((1.1)*x891))+(((0.09)*x899))+(((-0.09)*x901))+(((1.1)*x896))+(((-1.0)*(1.0)*pp))+(((1.1)*x894)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x902=py*py;
IkReal x903=(py*sj4);
IkReal x904=cj4*cj4;
IkReal x905=((0.045)*pz);
IkReal x906=(cj4*px);
IkReal x907=((0.3)*pz);
IkReal x908=((0.3)*cj4*px);
IkReal x909=((0.045)*x906);
IkReal x910=((0.3)*py*sj4);
IkReal x911=((0.045)*x903);
CheckValue<IkReal> x912 = IKatan2WithCheck(IkReal((((sj9*x911))+((sj9*x909))+((cj9*x910))+((sj9*x907))+x905+(((0.55)*x906))+(((0.55)*x903))+((cj9*x908))+(((-1.0)*cj9*x905)))),((((-1.0)*x911))+((cj9*x907))+((cj9*x911))+((cj9*x909))+(((-1.0)*sj9*x910))+(((0.55)*pz))+(((-1.0)*sj9*x908))+(((-1.0)*x909))+((sj9*x905))),IKFAST_ATAN2_MAGTHRESH);
if(!x912.valid){
continue;
}
CheckValue<IkReal> x913=IKPowWithIntegerCheck(IKsign(((((2.0)*cj4*px*x903))+(pz*pz)+x902+((x904*(px*px)))+(((-1.0)*x902*x904)))),-1);
if(!x913.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x912.value)+(((1.5707963267949)*(x913.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x914=((0.3)*cj9);
IkReal x915=((0.045)*sj9);
IkReal x916=IKcos(j6);
IkReal x917=(pz*x916);
IkReal x918=IKsin(j6);
IkReal x919=(cj4*px);
IkReal x920=(x918*x919);
IkReal x921=(py*sj4);
IkReal x922=(x918*x921);
IkReal x923=((0.045)*cj9);
IkReal x924=((0.3)*sj9);
IkReal x925=(pz*x918);
IkReal x926=(x916*x919);
IkReal x927=(x916*x921);
evalcond[0]=((-0.55)+(((-1.0)*x914))+x917+(((-1.0)*x915))+x922+x920);
evalcond[1]=((0.045)+(((-1.0)*x925))+(((-1.0)*x923))+x927+x924+x926);
evalcond[2]=((((-0.316735294117647)*x916))+(((-3.92156862745098)*pp*x918))+(((-0.588235294117647)*pp*x916))+pz+(((-0.108264705882353)*cj9*x916))+(((1.32323529411765)*cj9*x918))+(((1.51009803921569)*x918)));
evalcond[3]=((((0.55)*x918))+(((-0.045)*x916))+((x914*x918))+((x915*x918))+(((-1.0)*x921))+((x916*x923))+(((-1.0)*x919))+(((-1.0)*x916*x924)));
evalcond[4]=((-0.2125)+(((-0.09)*x927))+(((1.1)*x920))+(((0.09)*x925))+(((-0.09)*x926))+(((1.1)*x917))+(((1.1)*x922))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x928=(pz*sj8);
IkReal x929=((0.3)*cj9);
IkReal x930=((0.045)*sj9);
IkReal x931=(cj4*px);
IkReal x932=((0.045)*cj8*sj8);
IkReal x933=(x931*x932);
IkReal x934=(py*sj4);
IkReal x935=(x932*x934);
IkReal x936=((0.3)*cj8*sj8*sj9);
IkReal x937=((0.55)*cj8);
IkReal x938=(cj4*py);
IkReal x939=(px*sj4);
IkReal x940=(cj4*cj8*py);
IkReal x941=((1.0)*pz*sj8);
IkReal x942=(cj8*px*sj4);
IkReal x943=cj8*cj8;
IkReal x944=((0.045)*x943);
IkReal x945=(x938*x944);
IkReal x946=(x939*x944);
IkReal x947=((0.3)*sj9*x943);
CheckValue<IkReal> x948=IKPowWithIntegerCheck(IKsign(((((-0.55)*x928))+(((-1.0)*cj9*x933))+(((-1.0)*cj9*x935))+(((-1.0)*x928*x929))+((x931*x936))+(((-1.0)*x928*x930))+x933+x935+((x934*x936)))),-1);
if(!x948.valid){
continue;
}
CheckValue<IkReal> x949 = IKatan2WithCheck(IkReal(((((-1.0)*x937*x939))+((x937*x938))+((x930*x940))+(((-1.0)*x930*x942))+(((-1.0)*x931*x941))+(((-1.0)*x934*x941))+(((-1.0)*x929*x942))+((x929*x940)))),(((cj9*x946))+(((-1.0)*(1.0)*sj8*(pz*pz)))+((x938*x947))+(((-1.0)*x939*x947))+(((-1.0)*cj9*x945))+(((-1.0)*x946))+x945),IKFAST_ATAN2_MAGTHRESH);
if(!x949.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x948.value)))+(x949.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x950=((0.3)*cj9);
IkReal x951=((0.045)*sj9);
IkReal x952=IKcos(j6);
IkReal x953=(pz*x952);
IkReal x954=IKsin(j6);
IkReal x955=(cj4*px*x954);
IkReal x956=(py*sj4);
IkReal x957=(x954*x956);
IkReal x958=(px*sj4);
IkReal x959=((1.0)*cj4*py);
IkReal x960=(cj4*sj8);
IkReal x961=((0.045)*cj8);
IkReal x962=((0.045)*cj9);
IkReal x963=(cj8*x954);
IkReal x964=((0.3)*sj9);
IkReal x965=(sj8*x958);
IkReal x966=(pz*x963);
IkReal x967=(px*(((1.0)*cj4)));
IkReal x968=(cj8*x952);
IkReal x969=((1.0)*x956);
IkReal x970=((0.09)*cj8*x952);
evalcond[0]=((-0.55)+x953+x955+x957+(((-1.0)*x951))+(((-1.0)*x950)));
evalcond[1]=((((-1.0)*cj8*x959))+((cj8*x958))+(((-1.0)*pz*sj8*x954))+((sj8*x952*x956))+((px*x952*x960)));
evalcond[2]=((((-0.55)*x952))+pz+(((-1.0)*x962*x963))+(((-1.0)*x951*x952))+((x954*x961))+((x963*x964))+(((-1.0)*x950*x952)));
evalcond[3]=((0.045)+(((-1.0)*x968*x969))+(((-1.0)*sj8*x959))+x966+x964+x965+(((-1.0)*x962))+(((-1.0)*x967*x968)));
evalcond[4]=(((x952*x961))+((x964*x968))+(((0.55)*x954))+(((-1.0)*x962*x968))+(((-1.0)*x969))+((x951*x954))+((x950*x954))+(((-1.0)*x967)));
evalcond[5]=((-0.2125)+(((1.1)*x957))+(((1.1)*x953))+(((-0.09)*x965))+(((1.1)*x955))+(((0.09)*py*x960))+((cj4*px*x970))+((x956*x970))+(((-0.09)*x966))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x971=py*py;
IkReal x972=(sj8*x971);
IkReal x973=(cj4*px*sj8);
IkReal x974=cj4*cj4;
IkReal x975=px*px;
IkReal x976=((0.55)*sj8);
IkReal x977=(cj8*px);
IkReal x978=((0.3)*cj9);
IkReal x979=((0.045)*sj9);
IkReal x980=(py*sj4*sj8);
IkReal x981=(pz*sj8);
IkReal x982=(cj4*cj8*sj4);
CheckValue<IkReal> x983 = IKatan2WithCheck(IkReal((((x973*x978))+((x973*x979))+((pz*sj4*x977))+(((-1.0)*cj4*cj8*py*pz))+((cj4*px*x976))+((x979*x980))+((x978*x980))+((py*sj4*x976)))),(((pz*x976))+(((2.0)*py*x974*x977))+((x979*x981))+((x971*x982))+(((-1.0)*py*x977))+((x978*x981))+(((-1.0)*x975*x982))),IKFAST_ATAN2_MAGTHRESH);
if(!x983.valid){
continue;
}
CheckValue<IkReal> x984=IKPowWithIntegerCheck(IKsign(((((2.0)*py*sj4*x973))+((sj8*(pz*pz)))+x972+(((-1.0)*x972*x974))+((sj8*x974*x975)))),-1);
if(!x984.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x983.value)+(((1.5707963267949)*(x984.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x985=((0.3)*cj9);
IkReal x986=((0.045)*sj9);
IkReal x987=IKcos(j6);
IkReal x988=(pz*x987);
IkReal x989=IKsin(j6);
IkReal x990=(cj4*px*x989);
IkReal x991=(py*sj4);
IkReal x992=(x989*x991);
IkReal x993=(px*sj4);
IkReal x994=((1.0)*cj4*py);
IkReal x995=(cj4*sj8);
IkReal x996=((0.045)*cj8);
IkReal x997=((0.045)*cj9);
IkReal x998=(cj8*x989);
IkReal x999=((0.3)*sj9);
IkReal x1000=(sj8*x993);
IkReal x1001=(pz*x998);
IkReal x1002=(px*(((1.0)*cj4)));
IkReal x1003=(cj8*x987);
IkReal x1004=((1.0)*x991);
IkReal x1005=((0.09)*cj8*x987);
evalcond[0]=((-0.55)+(((-1.0)*x985))+(((-1.0)*x986))+x988+x990+x992);
evalcond[1]=((((-1.0)*cj8*x994))+(((-1.0)*pz*sj8*x989))+((px*x987*x995))+((sj8*x987*x991))+((cj8*x993)));
evalcond[2]=((((-1.0)*x986*x987))+pz+(((-0.55)*x987))+(((-1.0)*x997*x998))+((x998*x999))+(((-1.0)*x985*x987))+((x989*x996)));
evalcond[3]=((0.045)+(((-1.0)*sj8*x994))+(((-1.0)*x997))+x1000+x1001+(((-1.0)*x1002*x1003))+(((-1.0)*x1003*x1004))+x999);
evalcond[4]=((((-1.0)*x1003*x997))+((x985*x989))+((x1003*x999))+(((0.55)*x989))+(((-1.0)*x1004))+(((-1.0)*x1002))+((x986*x989))+((x987*x996)));
evalcond[5]=((-0.2125)+(((1.1)*x990))+((cj4*px*x1005))+((x1005*x991))+(((-0.09)*x1001))+(((0.09)*py*x995))+(((-0.09)*x1000))+(((-1.0)*(1.0)*pp))+(((1.1)*x988))+(((1.1)*x992)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1006=(cj4*px);
IkReal x1007=((0.045)*pz);
IkReal x1008=(py*sj4);
IkReal x1009=((0.3)*cj9);
IkReal x1010=((0.045)*sj9);
IkReal x1011=(cj8*cj9);
IkReal x1012=(cj8*sj9);
IkReal x1013=cj9*cj9;
IkReal x1014=((1.0)*pz);
CheckValue<IkReal> x1015 = IKatan2WithCheck(IkReal(((-0.304525)+(((-1.0)*(0.027)*cj9*sj9))+(((-1.0)*(0.0495)*sj9))+(pz*pz)+(((-0.087975)*x1013))+(((-1.0)*(0.33)*cj9)))),((((-0.087975)*sj9*x1011))+(((-1.0)*x1008*x1014))+(((-1.0)*x1006*x1014))+(((0.027)*cj8*x1013))+(((0.01125)*x1011))+(((-0.167025)*x1012))+(((-1.0)*(0.03825)*cj8))),IKFAST_ATAN2_MAGTHRESH);
if(!x1015.valid){
continue;
}
CheckValue<IkReal> x1016=IKPowWithIntegerCheck(IKsign(((((-1.0)*cj8*x1007))+((x1007*x1011))+(((-0.3)*pz*x1012))+(((-0.55)*x1008))+(((-1.0)*x1008*x1010))+(((-0.55)*x1006))+(((-1.0)*x1008*x1009))+(((-1.0)*x1006*x1009))+(((-1.0)*x1006*x1010)))),-1);
if(!x1016.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x1015.value)+(((1.5707963267949)*(x1016.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x1017=((0.3)*cj9);
IkReal x1018=((0.045)*sj9);
IkReal x1019=IKcos(j6);
IkReal x1020=(pz*x1019);
IkReal x1021=IKsin(j6);
IkReal x1022=(cj4*px*x1021);
IkReal x1023=(py*sj4);
IkReal x1024=(x1021*x1023);
IkReal x1025=(px*sj4);
IkReal x1026=((1.0)*cj4*py);
IkReal x1027=(cj4*sj8);
IkReal x1028=((0.045)*cj8);
IkReal x1029=((0.045)*cj9);
IkReal x1030=(cj8*x1021);
IkReal x1031=((0.3)*sj9);
IkReal x1032=(sj8*x1025);
IkReal x1033=(pz*x1030);
IkReal x1034=(px*(((1.0)*cj4)));
IkReal x1035=(cj8*x1019);
IkReal x1036=((1.0)*x1023);
IkReal x1037=((0.09)*cj8*x1019);
evalcond[0]=((-0.55)+x1020+x1022+x1024+(((-1.0)*x1018))+(((-1.0)*x1017)));
evalcond[1]=((((-1.0)*cj8*x1026))+(((-1.0)*pz*sj8*x1021))+((px*x1019*x1027))+((cj8*x1025))+((sj8*x1019*x1023)));
evalcond[2]=((((-1.0)*x1018*x1019))+(((-1.0)*x1029*x1030))+((x1030*x1031))+((x1021*x1028))+(((-0.55)*x1019))+pz+(((-1.0)*x1017*x1019)));
evalcond[3]=((0.045)+(((-1.0)*x1029))+(((-1.0)*x1035*x1036))+(((-1.0)*x1034*x1035))+x1031+x1033+x1032+(((-1.0)*sj8*x1026)));
evalcond[4]=(((x1031*x1035))+(((0.55)*x1021))+(((-1.0)*x1036))+((x1019*x1028))+((x1017*x1021))+((x1018*x1021))+(((-1.0)*x1029*x1035))+(((-1.0)*x1034)));
evalcond[5]=((-0.2125)+((cj4*px*x1037))+((x1023*x1037))+(((1.1)*x1024))+(((-0.09)*x1032))+(((0.09)*py*x1027))+(((1.1)*x1020))+(((-1.0)*(1.0)*pp))+(((1.1)*x1022))+(((-0.09)*x1033)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}
}
}

}

}

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x1038=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
IkReal x1039=((0.045)*cj8);
IkReal x1040=((((-1.0)*cj9*x1039))+x1039+(((0.3)*cj8*sj9)));
CheckValue<IkReal> x1043 = IKatan2WithCheck(IkReal(x1038),x1040,IKFAST_ATAN2_MAGTHRESH);
if(!x1043.valid){
continue;
}
IkReal x1041=((-1.0)*(x1043.value));
if((((x1040*x1040)+(x1038*x1038))) < -0.00001)
continue;
CheckValue<IkReal> x1044=IKPowWithIntegerCheck(IKabs(IKsqrt(((x1040*x1040)+(x1038*x1038)))),-1);
if(!x1044.valid){
continue;
}
if( ((pz*(x1044.value))) < -1-IKFAST_SINCOS_THRESH || ((pz*(x1044.value))) > 1+IKFAST_SINCOS_THRESH )
    continue;
IkReal x1042=IKasin((pz*(x1044.value)));
j6array[0]=((((-1.0)*x1042))+x1041);
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x1042+x1041);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];

{
IkReal j4eval[2];
IkReal x1045=((((-1.0)*(1.0)*sj6*(pz*pz)))+((pp*sj6)));
j4eval[0]=x1045;
j4eval[1]=IKsign(x1045);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
IkReal x1046=(cj8*sj6);
IkReal x1047=(((x1046*(pz*pz)))+(((-1.0)*pp*x1046)));
j4eval[0]=x1047;
j4eval[1]=IKsign(x1047);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
IkReal x1048=(pp+(((-1.0)*(1.0)*(pz*pz))));
j4eval[0]=x1048;
j4eval[1]=IKsign(x1048);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j8)))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=((((-1.0)*(0.3)*cj6*cj9))+(((-1.0)*(0.55)*cj6))+pz+(((-1.0)*(0.045)*cj6*sj9)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1049=((((-1.0)*(1.0)*cj6*(pz*pz)))+((cj6*pp)));
IkReal x1050=((1.51009803921569)*cj6);
IkReal x1051=(pz*sj6);
IkReal x1052=((1.32323529411765)*cj6*cj9);
IkReal x1053=((3.92156862745098)*cj6*pp);
j4eval[0]=x1049;
j4eval[1]=IKsign(x1049);
j4eval[2]=((IKabs((((py*x1053))+(((-1.0)*py*x1052))+(((-1.0)*py*x1050))+((px*x1051)))))+(IKabs((((px*x1052))+((px*x1050))+((py*x1051))+(((-1.0)*px*x1053))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1054=(cj6*pp);
IkReal x1055=(cj6*(pz*pz));
IkReal x1056=((0.2125)*cj6);
IkReal x1057=((1.1)*pz);
IkReal x1058=((0.09)*pz*sj6);
j4eval[0]=((((-1.0)*x1055))+x1054);
j4eval[1]=IKsign(((((-0.09)*x1055))+(((0.09)*x1054))));
j4eval[2]=((IKabs((((py*x1058))+((px*x1057))+(((-1.0)*px*x1054))+(((-1.0)*px*x1056)))))+(IKabs((((py*x1056))+((py*x1054))+(((-1.0)*py*x1057))+((px*x1058))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1059=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1060=((1.32323529411765)*cj9);
IkReal x1061=((3.92156862745098)*pp);
IkReal x1062=((0.316735294117647)*sj6);
IkReal x1063=((0.108264705882353)*cj9*sj6);
IkReal x1064=((0.588235294117647)*pp*sj6);
j4eval[0]=x1059;
j4eval[1]=((IKabs(((((-1.0)*(1.51009803921569)*py))+((px*x1064))+(((-1.0)*py*x1060))+((px*x1063))+((py*x1061))+((px*x1062)))))+(IKabs((((px*x1060))+(((-1.0)*px*x1061))+(((1.51009803921569)*px))+((py*x1063))+((py*x1062))+((py*x1064))))));
j4eval[2]=IKsign(x1059);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[7];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j6)))), 6.28318530717959)));
evalcond[1]=((-1.0)*(((1.0)*pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1065=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1066=(cj9*px);
IkReal x1067=(cj9*py);
IkReal x1068=((3.92156862745098)*pp);
IkReal x1069=((0.045)*sj9);
j4eval[0]=x1065;
j4eval[1]=((IKabs(((((1.51009803921569)*px))+((py*x1069))+(((0.55)*py))+(((0.3)*x1067))+(((1.32323529411765)*x1066))+(((-1.0)*px*x1068)))))+(IKabs(((((0.3)*x1066))+(((-1.0)*(1.51009803921569)*py))+(((-1.32323529411765)*x1067))+(((0.55)*px))+((py*x1068))+((px*x1069))))));
j4eval[2]=IKsign(x1065);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1070=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1071=(cj9*px);
IkReal x1072=(cj9*py);
IkReal x1073=(pp*px);
IkReal x1074=(pp*py);
j4eval[0]=x1070;
j4eval[1]=((IKabs(((((0.108264705882353)*x1072))+(((0.588235294117647)*x1074))+(((1.32323529411765)*x1071))+(((0.316735294117647)*py))+(((1.51009803921569)*px))+(((-3.92156862745098)*x1073)))))+(IKabs(((((-1.32323529411765)*x1072))+(((0.588235294117647)*x1073))+(((3.92156862745098)*x1074))+(((0.316735294117647)*px))+(((0.108264705882353)*x1071))+(((-1.0)*(1.51009803921569)*py))))));
j4eval[2]=IKsign(x1070);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1075=pz*pz;
IkReal x1076=(cj9*px);
IkReal x1077=(cj9*py);
IkReal x1078=(pp*px);
IkReal x1079=(pp*py);
j4eval[0]=((((-1.0)*x1075))+pp);
j4eval[1]=((IKabs(((((0.348408823529412)*py))+(((1.66110784313725)*px))+(((1.45555882352941)*x1076))+(((0.647058823529412)*x1079))+(((-4.31372549019608)*x1078))+(((0.119091176470588)*x1077)))))+(IKabs(((((0.348408823529412)*px))+(((-1.45555882352941)*x1077))+(((4.31372549019608)*x1079))+(((0.647058823529412)*x1078))+(((-1.0)*(1.66110784313725)*py))+(((0.119091176470588)*x1076))))));
j4eval[2]=IKsign(((((1.1)*pp))+(((-1.1)*x1075))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
IkReal x1080=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=x1080;
evalcond[3]=x1080;
evalcond[4]=((0.316735294117647)+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp)));
evalcond[5]=((-0.2125)+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1081=(cj9*px);
IkReal x1082=(cj9*py);
IkReal x1083=(pp*px);
IkReal x1084=(pp*py);
CheckValue<IkReal> x1085 = IKatan2WithCheck(IkReal(((((0.119091176470588)*x1082))+(((0.348408823529412)*py))+(((1.66110784313725)*px))+(((0.647058823529412)*x1084))+(((-4.31372549019608)*x1083))+(((1.45555882352941)*x1081)))),((((4.31372549019608)*x1084))+(((0.119091176470588)*x1081))+(((0.348408823529412)*px))+(((0.647058823529412)*x1083))+(((-1.45555882352941)*x1082))+(((-1.0)*(1.66110784313725)*py))),IKFAST_ATAN2_MAGTHRESH);
if(!x1085.valid){
continue;
}
CheckValue<IkReal> x1086=IKPowWithIntegerCheck(IKsign(((((1.1)*pp))+(((-1.0)*(1.1)*(pz*pz))))),-1);
if(!x1086.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1085.value)+(((1.5707963267949)*(x1086.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1087=IKcos(j4);
IkReal x1088=(px*x1087);
IkReal x1089=IKsin(j4);
IkReal x1090=(py*x1089);
IkReal x1091=(px*x1089);
IkReal x1092=(py*x1087);
evalcond[0]=((-0.55)+x1088+(((-1.0)*(0.3)*cj9))+x1090+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*x1092))+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1091);
evalcond[2]=((0.316735294117647)+(((-1.0)*x1090))+(((-1.0)*x1088))+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp)));
evalcond[3]=((-0.2125)+(((1.1)*x1090))+(((0.09)*x1092))+(((-0.09)*x1091))+(((1.1)*x1088))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1093=(cj9*px);
IkReal x1094=(cj9*py);
IkReal x1095=(pp*px);
IkReal x1096=(pp*py);
CheckValue<IkReal> x1097 = IKatan2WithCheck(IkReal(((((-3.92156862745098)*x1095))+(((0.316735294117647)*py))+(((1.51009803921569)*px))+(((0.108264705882353)*x1094))+(((1.32323529411765)*x1093))+(((0.588235294117647)*x1096)))),((((0.108264705882353)*x1093))+(((0.588235294117647)*x1095))+(((-1.32323529411765)*x1094))+(((0.316735294117647)*px))+(((-1.0)*(1.51009803921569)*py))+(((3.92156862745098)*x1096))),IKFAST_ATAN2_MAGTHRESH);
if(!x1097.valid){
continue;
}
CheckValue<IkReal> x1098=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1098.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1097.value)+(((1.5707963267949)*(x1098.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1099=IKcos(j4);
IkReal x1100=(px*x1099);
IkReal x1101=IKsin(j4);
IkReal x1102=(py*x1101);
IkReal x1103=(px*x1101);
IkReal x1104=(py*x1099);
evalcond[0]=((-0.55)+x1102+x1100+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*x1104))+(((-1.0)*(1.32323529411765)*cj9))+x1103+(((3.92156862745098)*pp)));
evalcond[2]=((0.316735294117647)+(((-1.0)*x1102))+(((-1.0)*x1100))+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp)));
evalcond[3]=((-0.2125)+(((-0.09)*x1103))+(((1.1)*x1100))+(((1.1)*x1102))+(((-1.0)*(1.0)*pp))+(((0.09)*x1104)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1105=(cj9*px);
IkReal x1106=(cj9*py);
IkReal x1107=((3.92156862745098)*pp);
IkReal x1108=((0.045)*sj9);
CheckValue<IkReal> x1109=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1109.valid){
continue;
}
CheckValue<IkReal> x1110 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1107))+((py*x1108))+(((1.51009803921569)*px))+(((0.55)*py))+(((1.32323529411765)*x1105))+(((0.3)*x1106)))),((((-1.32323529411765)*x1106))+(((-1.0)*(1.51009803921569)*py))+((py*x1107))+(((0.55)*px))+((px*x1108))+(((0.3)*x1105))),IKFAST_ATAN2_MAGTHRESH);
if(!x1110.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1109.value)))+(x1110.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1111=IKcos(j4);
IkReal x1112=(px*x1111);
IkReal x1113=IKsin(j4);
IkReal x1114=(py*x1113);
IkReal x1115=(px*x1113);
IkReal x1116=(py*x1111);
evalcond[0]=((-0.55)+x1112+x1114+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((-1.51009803921569)+x1115+(((-1.0)*(1.32323529411765)*cj9))+(((-1.0)*x1116))+(((3.92156862745098)*pp)));
evalcond[2]=((0.316735294117647)+(((-1.0)*x1114))+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp))+(((-1.0)*x1112)));
evalcond[3]=((-0.2125)+(((-0.09)*x1115))+(((1.1)*x1114))+(((1.1)*x1112))+(((-1.0)*(1.0)*pp))+(((0.09)*x1116)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j6)))), 6.28318530717959)));
evalcond[1]=pz;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1117=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1118=(cj9*px);
IkReal x1119=(cj9*py);
IkReal x1120=((3.92156862745098)*pp);
IkReal x1121=((0.045)*sj9);
j4eval[0]=x1117;
j4eval[1]=((IKabs(((((-0.3)*x1119))+(((1.51009803921569)*px))+(((1.32323529411765)*x1118))+(((-1.0)*px*x1120))+(((-1.0)*py*x1121))+(((-1.0)*(0.55)*py)))))+(IKabs(((((-1.0)*px*x1121))+(((-1.0)*(0.55)*px))+(((-0.3)*x1118))+(((-1.0)*(1.51009803921569)*py))+((py*x1120))+(((-1.32323529411765)*x1119))))));
j4eval[2]=IKsign(x1117);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1122=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1123=(cj9*px);
IkReal x1124=(cj9*py);
IkReal x1125=(pp*px);
IkReal x1126=(pp*py);
j4eval[0]=x1122;
j4eval[1]=((IKabs(((((-0.108264705882353)*x1124))+(((1.51009803921569)*px))+(((-1.0)*(0.316735294117647)*py))+(((-3.92156862745098)*x1125))+(((-0.588235294117647)*x1126))+(((1.32323529411765)*x1123)))))+(IKabs(((((-0.108264705882353)*x1123))+(((-1.0)*(1.51009803921569)*py))+(((-1.0)*(0.316735294117647)*px))+(((-0.588235294117647)*x1125))+(((3.92156862745098)*x1126))+(((-1.32323529411765)*x1124))))));
j4eval[2]=IKsign(x1122);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=1.0;
cj8=0;
j8=1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1127=pz*pz;
IkReal x1128=(cj9*px);
IkReal x1129=(cj9*py);
IkReal x1130=(pp*px);
IkReal x1131=(pp*py);
j4eval[0]=(x1127+(((-1.0)*(1.0)*pp)));
j4eval[1]=IKsign(((((1.1)*x1127))+(((-1.0)*(1.1)*pp))));
j4eval[2]=((IKabs(((((0.348408823529412)*px))+(((1.66110784313725)*py))+(((0.119091176470588)*x1128))+(((0.647058823529412)*x1130))+(((1.45555882352941)*x1129))+(((-4.31372549019608)*x1131)))))+(IKabs(((((0.647058823529412)*x1131))+(((0.348408823529412)*py))+(((0.119091176470588)*x1129))+(((4.31372549019608)*x1130))+(((-1.0)*(1.66110784313725)*px))+(((-1.45555882352941)*x1128))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
IkReal x1132=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=x1132;
evalcond[3]=x1132;
evalcond[4]=((-0.316735294117647)+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9)));
evalcond[5]=((-0.2125)+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1133=(cj9*px);
IkReal x1134=(cj9*py);
IkReal x1135=(pp*px);
IkReal x1136=(pp*py);
CheckValue<IkReal> x1137 = IKatan2WithCheck(IkReal(((((4.31372549019608)*x1135))+(((-1.45555882352941)*x1133))+(((0.348408823529412)*py))+(((0.119091176470588)*x1134))+(((-1.0)*(1.66110784313725)*px))+(((0.647058823529412)*x1136)))),((((0.348408823529412)*px))+(((0.119091176470588)*x1133))+(((1.66110784313725)*py))+(((1.45555882352941)*x1134))+(((-4.31372549019608)*x1136))+(((0.647058823529412)*x1135))),IKFAST_ATAN2_MAGTHRESH);
if(!x1137.valid){
continue;
}
CheckValue<IkReal> x1138=IKPowWithIntegerCheck(IKsign(((((1.1)*(pz*pz)))+(((-1.0)*(1.1)*pp)))),-1);
if(!x1138.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1137.value)+(((1.5707963267949)*(x1138.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1139=IKsin(j4);
IkReal x1140=(px*x1139);
IkReal x1141=IKcos(j4);
IkReal x1142=(py*x1141);
IkReal x1143=(px*x1141);
IkReal x1144=(py*x1139);
IkReal x1145=((((-1.0)*x1144))+(((-1.0)*x1143)));
evalcond[0]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+x1140+(((3.92156862745098)*pp))+(((-1.0)*x1142)));
evalcond[1]=((-0.55)+x1145+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((-0.316735294117647)+x1145+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9)));
evalcond[3]=((-0.2125)+(((-0.09)*x1140))+(((-1.1)*x1144))+(((-1.1)*x1143))+(((0.09)*x1142))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1146=(cj9*px);
IkReal x1147=(cj9*py);
IkReal x1148=(pp*px);
IkReal x1149=(pp*py);
CheckValue<IkReal> x1150 = IKatan2WithCheck(IkReal(((((1.51009803921569)*px))+(((-1.0)*(0.316735294117647)*py))+(((-0.108264705882353)*x1147))+(((-0.588235294117647)*x1149))+(((1.32323529411765)*x1146))+(((-3.92156862745098)*x1148)))),((((-0.108264705882353)*x1146))+(((-1.32323529411765)*x1147))+(((3.92156862745098)*x1149))+(((-1.0)*(1.51009803921569)*py))+(((-1.0)*(0.316735294117647)*px))+(((-0.588235294117647)*x1148))),IKFAST_ATAN2_MAGTHRESH);
if(!x1150.valid){
continue;
}
CheckValue<IkReal> x1151=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1151.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1150.value)+(((1.5707963267949)*(x1151.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1152=IKsin(j4);
IkReal x1153=(px*x1152);
IkReal x1154=IKcos(j4);
IkReal x1155=(py*x1154);
IkReal x1156=(px*x1154);
IkReal x1157=(py*x1152);
IkReal x1158=((((-1.0)*x1156))+(((-1.0)*x1157)));
evalcond[0]=((-1.51009803921569)+(((-1.0)*x1155))+x1153+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[1]=((-0.55)+x1158+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((-0.316735294117647)+x1158+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9)));
evalcond[3]=((-0.2125)+(((-1.1)*x1156))+(((-1.1)*x1157))+(((-0.09)*x1153))+(((-1.0)*(1.0)*pp))+(((0.09)*x1155)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1159=(cj9*px);
IkReal x1160=(cj9*py);
IkReal x1161=((3.92156862745098)*pp);
IkReal x1162=((0.045)*sj9);
CheckValue<IkReal> x1163=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1163.valid){
continue;
}
CheckValue<IkReal> x1164 = IKatan2WithCheck(IkReal(((((-0.3)*x1160))+(((1.51009803921569)*px))+(((-1.0)*py*x1162))+(((-1.0)*px*x1161))+(((1.32323529411765)*x1159))+(((-1.0)*(0.55)*py)))),((((-1.0)*(0.55)*px))+(((-1.32323529411765)*x1160))+(((-1.0)*(1.51009803921569)*py))+(((-0.3)*x1159))+((py*x1161))+(((-1.0)*px*x1162))),IKFAST_ATAN2_MAGTHRESH);
if(!x1164.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1163.value)))+(x1164.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1165=IKsin(j4);
IkReal x1166=(px*x1165);
IkReal x1167=IKcos(j4);
IkReal x1168=(py*x1167);
IkReal x1169=(px*x1167);
IkReal x1170=(py*x1165);
IkReal x1171=((((-1.0)*x1169))+(((-1.0)*x1170)));
evalcond[0]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((-1.0)*x1168))+(((3.92156862745098)*pp))+x1166);
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1171+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((-0.316735294117647)+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9))+x1171);
evalcond[3]=((-0.2125)+(((-1.1)*x1170))+(((-1.1)*x1169))+(((0.09)*x1168))+(((-0.09)*x1166))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1172=(cj6*pz);
IkReal x1173=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1172+(((-1.0)*(0.045)*sj9)));
evalcond[2]=x1173;
evalcond[3]=((-1.0)*(((1.0)*pz*sj6)));
evalcond[4]=x1173;
evalcond[5]=((((0.108264705882353)*cj9*sj6))+(((0.588235294117647)*pp*sj6))+(((0.316735294117647)*sj6)));
evalcond[6]=((-0.2125)+(((1.1)*x1172))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1174=((1.32323529411765)*cj9);
IkReal x1175=((3.92156862745098)*pp);
IkReal x1176=((0.316735294117647)*sj6);
IkReal x1177=((0.108264705882353)*cj9*sj6);
IkReal x1178=((0.588235294117647)*pp*sj6);
CheckValue<IkReal> x1179 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1175))+((px*x1174))+(((1.51009803921569)*px))+((py*x1178))+((py*x1177))+((py*x1176)))),(((px*x1176))+(((-1.0)*(1.51009803921569)*py))+((px*x1178))+((py*x1175))+(((-1.0)*py*x1174))+((px*x1177))),IKFAST_ATAN2_MAGTHRESH);
if(!x1179.valid){
continue;
}
CheckValue<IkReal> x1180=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1180.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1179.value)+(((1.5707963267949)*(x1180.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1181=IKcos(j4);
IkReal x1182=(px*x1181);
IkReal x1183=IKsin(j4);
IkReal x1184=(py*x1183);
IkReal x1185=(px*x1183);
IkReal x1186=((1.0)*x1181);
IkReal x1187=(cj6*pz);
IkReal x1188=(sj6*x1182);
IkReal x1189=(sj6*x1184);
evalcond[0]=(((cj6*x1184))+((cj6*x1182))+(((-1.0)*(1.0)*pz*sj6)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*py*x1186))+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1185);
evalcond[2]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1187+x1188+x1189+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((0.108264705882353)*cj9*sj6))+(((-1.0)*x1184))+(((-1.0)*px*x1186))+(((0.588235294117647)*pp*sj6))+(((0.316735294117647)*sj6)));
evalcond[4]=((-0.2125)+(((1.1)*x1188))+(((-0.09)*x1185))+(((0.09)*py*x1181))+(((-1.0)*(1.0)*pp))+(((1.1)*x1187))+(((1.1)*x1189)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1190=(cj6*pp);
IkReal x1191=((0.2125)*cj6);
IkReal x1192=((1.1)*pz);
IkReal x1193=((0.09)*pz*sj6);
CheckValue<IkReal> x1194 = IKatan2WithCheck(IkReal((((px*x1192))+(((-1.0)*px*x1190))+((py*x1193))+(((-1.0)*px*x1191)))),(((py*x1190))+(((-1.0)*py*x1192))+((py*x1191))+((px*x1193))),IKFAST_ATAN2_MAGTHRESH);
if(!x1194.valid){
continue;
}
CheckValue<IkReal> x1195=IKPowWithIntegerCheck(IKsign(((((0.09)*x1190))+(((-1.0)*(0.09)*cj6*(pz*pz))))),-1);
if(!x1195.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1194.value)+(((1.5707963267949)*(x1195.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1196=IKcos(j4);
IkReal x1197=(px*x1196);
IkReal x1198=IKsin(j4);
IkReal x1199=(py*x1198);
IkReal x1200=(px*x1198);
IkReal x1201=((1.0)*x1196);
IkReal x1202=(cj6*pz);
IkReal x1203=(sj6*x1197);
IkReal x1204=(sj6*x1199);
evalcond[0]=(((cj6*x1199))+((cj6*x1197))+(((-1.0)*(1.0)*pz*sj6)));
evalcond[1]=((-1.51009803921569)+x1200+(((-1.0)*(1.32323529411765)*cj9))+(((-1.0)*py*x1201))+(((3.92156862745098)*pp)));
evalcond[2]=((-0.55)+x1202+x1203+x1204+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((0.108264705882353)*cj9*sj6))+(((0.588235294117647)*pp*sj6))+(((-1.0)*px*x1201))+(((0.316735294117647)*sj6))+(((-1.0)*x1199)));
evalcond[4]=((-0.2125)+(((1.1)*x1204))+(((1.1)*x1203))+(((-0.09)*x1200))+(((0.09)*py*x1196))+(((1.1)*x1202))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1205=((1.51009803921569)*cj6);
IkReal x1206=(pz*sj6);
IkReal x1207=((1.32323529411765)*cj6*cj9);
IkReal x1208=((3.92156862745098)*cj6*pp);
CheckValue<IkReal> x1209 = IKatan2WithCheck(IkReal((((px*x1205))+((px*x1207))+((py*x1206))+(((-1.0)*px*x1208)))),((((-1.0)*py*x1205))+((py*x1208))+((px*x1206))+(((-1.0)*py*x1207))),IKFAST_ATAN2_MAGTHRESH);
if(!x1209.valid){
continue;
}
CheckValue<IkReal> x1210=IKPowWithIntegerCheck(IKsign(((((-1.0)*(1.0)*cj6*(pz*pz)))+((cj6*pp)))),-1);
if(!x1210.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1209.value)+(((1.5707963267949)*(x1210.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1211=IKcos(j4);
IkReal x1212=(px*x1211);
IkReal x1213=IKsin(j4);
IkReal x1214=(py*x1213);
IkReal x1215=(px*x1213);
IkReal x1216=((1.0)*x1211);
IkReal x1217=(cj6*pz);
IkReal x1218=(sj6*x1212);
IkReal x1219=(sj6*x1214);
evalcond[0]=(((cj6*x1212))+((cj6*x1214))+(((-1.0)*(1.0)*pz*sj6)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1215+(((-1.0)*py*x1216)));
evalcond[2]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1218+x1219+x1217+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((0.108264705882353)*cj9*sj6))+(((-1.0)*x1214))+(((0.588235294117647)*pp*sj6))+(((-1.0)*px*x1216))+(((0.316735294117647)*sj6)));
evalcond[4]=((-0.2125)+(((1.1)*x1217))+(((1.1)*x1218))+(((-0.09)*x1215))+(((1.1)*x1219))+(((-1.0)*(1.0)*pp))+(((0.09)*py*x1211)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j8)))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=((((-1.0)*(0.3)*cj6*cj9))+(((-1.0)*(0.55)*cj6))+pz+(((-1.0)*(0.045)*cj6*sj9)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1220=((((-1.0)*(1.0)*cj6*(pz*pz)))+((cj6*pp)));
IkReal x1221=((1.51009803921569)*cj6);
IkReal x1222=(pz*sj6);
IkReal x1223=((1.32323529411765)*cj6*cj9);
IkReal x1224=((3.92156862745098)*cj6*pp);
j4eval[0]=x1220;
j4eval[1]=((IKabs((((py*x1221))+((py*x1223))+((px*x1222))+(((-1.0)*py*x1224)))))+(IKabs(((((-1.0)*px*x1221))+((py*x1222))+(((-1.0)*px*x1223))+((px*x1224))))));
j4eval[2]=IKsign(x1220);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1225=(cj6*pp);
IkReal x1226=((1.0)*x1225);
IkReal x1227=(cj6*(pz*pz));
IkReal x1228=((0.2125)*cj6);
IkReal x1229=((1.1)*pz);
IkReal x1230=((0.09)*pz*sj6);
j4eval[0]=((((-1.0)*x1226))+x1227);
j4eval[1]=IKsign(((((-0.09)*x1225))+(((0.09)*x1227))));
j4eval[2]=((IKabs((((py*x1228))+((py*x1225))+(((-1.0)*py*x1229))+(((-1.0)*px*x1230)))))+(IKabs(((((-1.0)*px*x1228))+(((-1.0)*px*x1226))+((px*x1229))+(((-1.0)*py*x1230))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1231=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1232=((1.32323529411765)*cj9);
IkReal x1233=((3.92156862745098)*pp);
IkReal x1234=((0.316735294117647)*sj6);
IkReal x1235=((0.108264705882353)*cj9*sj6);
IkReal x1236=((0.588235294117647)*pp*sj6);
j4eval[0]=x1231;
j4eval[1]=((IKabs((((px*x1233))+(((-1.0)*(1.51009803921569)*px))+(((-1.0)*px*x1232))+((py*x1234))+((py*x1236))+((py*x1235)))))+(IKabs(((((-1.0)*py*x1233))+((py*x1232))+((px*x1236))+((px*x1235))+(((1.51009803921569)*py))+((px*x1234))))));
j4eval[2]=IKsign(x1231);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[7];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j6)))), 6.28318530717959)));
evalcond[1]=pz;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1237=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1238=(cj9*px);
IkReal x1239=(cj9*py);
IkReal x1240=((3.92156862745098)*pp);
IkReal x1241=((0.045)*sj9);
j4eval[0]=x1237;
j4eval[1]=((IKabs((((px*x1240))+(((-1.0)*(1.51009803921569)*px))+(((0.55)*py))+(((0.3)*x1239))+((py*x1241))+(((-1.32323529411765)*x1238)))))+(IKabs((((px*x1241))+(((1.51009803921569)*py))+(((1.32323529411765)*x1239))+(((-1.0)*py*x1240))+(((0.55)*px))+(((0.3)*x1238))))));
j4eval[2]=IKsign(x1237);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1242=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1243=(cj9*px);
IkReal x1244=(cj9*py);
IkReal x1245=(pp*px);
IkReal x1246=(pp*py);
j4eval[0]=x1242;
j4eval[1]=((IKabs(((((-1.32323529411765)*x1243))+(((0.316735294117647)*py))+(((-1.0)*(1.51009803921569)*px))+(((0.108264705882353)*x1244))+(((0.588235294117647)*x1246))+(((3.92156862745098)*x1245)))))+(IKabs(((((0.588235294117647)*x1245))+(((1.32323529411765)*x1244))+(((-3.92156862745098)*x1246))+(((0.316735294117647)*px))+(((1.51009803921569)*py))+(((0.108264705882353)*x1243))))));
j4eval[2]=IKsign(x1242);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=1.0;
cj6=0;
j6=1.5707963267949;
IkReal x1247=pz*pz;
IkReal x1248=(cj9*px);
IkReal x1249=(cj9*py);
IkReal x1250=(pp*px);
IkReal x1251=(pp*py);
j4eval[0]=(pp+(((-1.0)*x1247)));
j4eval[1]=((IKabs(((((0.348408823529412)*py))+(((0.119091176470588)*x1249))+(((0.647058823529412)*x1251))+(((-1.45555882352941)*x1248))+(((4.31372549019608)*x1250))+(((-1.0)*(1.66110784313725)*px)))))+(IKabs(((((1.45555882352941)*x1249))+(((0.348408823529412)*px))+(((1.66110784313725)*py))+(((0.647058823529412)*x1250))+(((0.119091176470588)*x1248))+(((-4.31372549019608)*x1251))))));
j4eval[2]=IKsign(((((1.1)*pp))+(((-1.1)*x1247))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
IkReal x1252=((1.32323529411765)*cj9);
IkReal x1253=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((1.51009803921569)+(((-1.0)*x1253))+x1252);
evalcond[3]=((-1.51009803921569)+(((-1.0)*x1252))+x1253);
evalcond[4]=((0.316735294117647)+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp)));
evalcond[5]=((-0.2125)+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1254=(cj9*px);
IkReal x1255=(cj9*py);
IkReal x1256=(pp*px);
IkReal x1257=(pp*py);
CheckValue<IkReal> x1258 = IKatan2WithCheck(IkReal(((((0.348408823529412)*py))+(((0.647058823529412)*x1257))+(((4.31372549019608)*x1256))+(((0.119091176470588)*x1255))+(((-1.0)*(1.66110784313725)*px))+(((-1.45555882352941)*x1254)))),((((0.348408823529412)*px))+(((1.66110784313725)*py))+(((0.119091176470588)*x1254))+(((-4.31372549019608)*x1257))+(((0.647058823529412)*x1256))+(((1.45555882352941)*x1255))),IKFAST_ATAN2_MAGTHRESH);
if(!x1258.valid){
continue;
}
CheckValue<IkReal> x1259=IKPowWithIntegerCheck(IKsign(((((1.1)*pp))+(((-1.0)*(1.1)*(pz*pz))))),-1);
if(!x1259.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1258.value)+(((1.5707963267949)*(x1259.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1260=IKcos(j4);
IkReal x1261=(px*x1260);
IkReal x1262=IKsin(j4);
IkReal x1263=(py*x1262);
IkReal x1264=(px*x1262);
IkReal x1265=(py*x1260);
evalcond[0]=((-0.55)+x1261+x1263+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*x1265))+x1264+(((1.32323529411765)*cj9)));
evalcond[2]=((0.316735294117647)+(((-1.0)*x1261))+(((-1.0)*x1263))+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp)));
evalcond[3]=((-0.2125)+(((0.09)*x1264))+(((-0.09)*x1265))+(((-1.0)*(1.0)*pp))+(((1.1)*x1263))+(((1.1)*x1261)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1266=(cj9*px);
IkReal x1267=(cj9*py);
IkReal x1268=(pp*px);
IkReal x1269=(pp*py);
CheckValue<IkReal> x1270 = IKatan2WithCheck(IkReal(((((0.316735294117647)*py))+(((0.108264705882353)*x1267))+(((-1.32323529411765)*x1266))+(((3.92156862745098)*x1268))+(((-1.0)*(1.51009803921569)*px))+(((0.588235294117647)*x1269)))),((((0.588235294117647)*x1268))+(((1.32323529411765)*x1267))+(((0.316735294117647)*px))+(((1.51009803921569)*py))+(((-3.92156862745098)*x1269))+(((0.108264705882353)*x1266))),IKFAST_ATAN2_MAGTHRESH);
if(!x1270.valid){
continue;
}
CheckValue<IkReal> x1271=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1271.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1270.value)+(((1.5707963267949)*(x1271.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1272=IKcos(j4);
IkReal x1273=(px*x1272);
IkReal x1274=IKsin(j4);
IkReal x1275=(py*x1274);
IkReal x1276=(px*x1274);
IkReal x1277=(py*x1272);
evalcond[0]=((-0.55)+x1275+x1273+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+x1276+(((1.32323529411765)*cj9))+(((-1.0)*x1277)));
evalcond[2]=((0.316735294117647)+(((0.108264705882353)*cj9))+(((-1.0)*x1273))+(((0.588235294117647)*pp))+(((-1.0)*x1275)));
evalcond[3]=((-0.2125)+(((0.09)*x1276))+(((1.1)*x1275))+(((1.1)*x1273))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1277)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1278=(cj9*px);
IkReal x1279=(cj9*py);
IkReal x1280=((3.92156862745098)*pp);
IkReal x1281=((0.045)*sj9);
CheckValue<IkReal> x1282=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1282.valid){
continue;
}
CheckValue<IkReal> x1283 = IKatan2WithCheck(IkReal(((((-1.0)*(1.51009803921569)*px))+(((-1.32323529411765)*x1278))+(((0.55)*py))+((px*x1280))+((py*x1281))+(((0.3)*x1279)))),((((-1.0)*py*x1280))+(((1.32323529411765)*x1279))+(((1.51009803921569)*py))+(((0.3)*x1278))+(((0.55)*px))+((px*x1281))),IKFAST_ATAN2_MAGTHRESH);
if(!x1283.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1282.value)))+(x1283.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1284=IKcos(j4);
IkReal x1285=(px*x1284);
IkReal x1286=IKsin(j4);
IkReal x1287=(py*x1286);
IkReal x1288=(px*x1286);
IkReal x1289=(py*x1284);
evalcond[0]=((-0.55)+x1287+x1285+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[1]=((1.51009803921569)+(((-1.0)*x1289))+(((-1.0)*(3.92156862745098)*pp))+x1288+(((1.32323529411765)*cj9)));
evalcond[2]=((0.316735294117647)+(((-1.0)*x1285))+(((0.108264705882353)*cj9))+(((0.588235294117647)*pp))+(((-1.0)*x1287)));
evalcond[3]=((-0.2125)+(((0.09)*x1288))+(((-0.09)*x1289))+(((1.1)*x1287))+(((-1.0)*(1.0)*pp))+(((1.1)*x1285)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j6)))), 6.28318530717959)));
evalcond[1]=((-1.0)*(((1.0)*pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1290=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1291=(cj9*px);
IkReal x1292=(cj9*py);
IkReal x1293=((3.92156862745098)*pp);
IkReal x1294=((0.045)*sj9);
j4eval[0]=x1290;
j4eval[1]=((IKabs(((((-1.32323529411765)*x1291))+((px*x1293))+(((-1.0)*(1.51009803921569)*px))+(((-1.0)*py*x1294))+(((-0.3)*x1292))+(((-1.0)*(0.55)*py)))))+(IKabs(((((-1.0)*(0.55)*px))+(((-0.3)*x1291))+(((-1.0)*px*x1294))+(((1.51009803921569)*py))+(((-1.0)*py*x1293))+(((1.32323529411765)*x1292))))));
j4eval[2]=IKsign(x1290);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1295=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1296=(cj9*px);
IkReal x1297=(cj9*py);
IkReal x1298=(pp*px);
IkReal x1299=(pp*py);
j4eval[0]=x1295;
j4eval[1]=((IKabs(((((-3.92156862745098)*x1299))+(((1.51009803921569)*py))+(((-0.108264705882353)*x1296))+(((-1.0)*(0.316735294117647)*px))+(((1.32323529411765)*x1297))+(((-0.588235294117647)*x1298)))))+(IKabs(((((-0.588235294117647)*x1299))+(((-1.32323529411765)*x1296))+(((-1.0)*(1.51009803921569)*px))+(((-1.0)*(0.316735294117647)*py))+(((-0.108264705882353)*x1297))+(((3.92156862745098)*x1298))))));
j4eval[2]=IKsign(x1295);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
sj6=-1.0;
cj6=0;
j6=-1.5707963267949;
IkReal x1300=pz*pz;
IkReal x1301=(cj9*px);
IkReal x1302=(cj9*py);
IkReal x1303=(pp*px);
IkReal x1304=(pp*py);
j4eval[0]=(x1300+(((-1.0)*(1.0)*pp)));
j4eval[1]=IKsign(((((1.1)*x1300))+(((-1.0)*(1.1)*pp))));
j4eval[2]=((IKabs(((((4.31372549019608)*x1304))+(((0.348408823529412)*px))+(((0.119091176470588)*x1301))+(((-1.0)*(1.66110784313725)*py))+(((0.647058823529412)*x1303))+(((-1.45555882352941)*x1302)))))+(IKabs(((((0.348408823529412)*py))+(((1.66110784313725)*px))+(((1.45555882352941)*x1301))+(((-4.31372549019608)*x1303))+(((0.647058823529412)*x1304))+(((0.119091176470588)*x1302))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
IkReal x1305=((1.32323529411765)*cj9);
IkReal x1306=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((1.51009803921569)+(((-1.0)*x1306))+x1305);
evalcond[3]=((-1.51009803921569)+x1306+(((-1.0)*x1305)));
evalcond[4]=((-0.316735294117647)+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9)));
evalcond[5]=((-0.2125)+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1307=(cj9*px);
IkReal x1308=(cj9*py);
IkReal x1309=(pp*px);
IkReal x1310=(pp*py);
CheckValue<IkReal> x1311=IKPowWithIntegerCheck(IKsign(((((1.1)*(pz*pz)))+(((-1.0)*(1.1)*pp)))),-1);
if(!x1311.valid){
continue;
}
CheckValue<IkReal> x1312 = IKatan2WithCheck(IkReal(((((1.45555882352941)*x1307))+(((0.348408823529412)*py))+(((1.66110784313725)*px))+(((0.119091176470588)*x1308))+(((-4.31372549019608)*x1309))+(((0.647058823529412)*x1310)))),((((0.647058823529412)*x1309))+(((0.348408823529412)*px))+(((4.31372549019608)*x1310))+(((-1.45555882352941)*x1308))+(((0.119091176470588)*x1307))+(((-1.0)*(1.66110784313725)*py))),IKFAST_ATAN2_MAGTHRESH);
if(!x1312.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1311.value)))+(x1312.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1313=IKsin(j4);
IkReal x1314=(px*x1313);
IkReal x1315=IKcos(j4);
IkReal x1316=(py*x1315);
IkReal x1317=(px*x1315);
IkReal x1318=(py*x1313);
IkReal x1319=((((-1.0)*x1318))+(((-1.0)*x1317)));
evalcond[0]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*x1316))+(((1.32323529411765)*cj9))+x1314);
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9))+x1319);
evalcond[2]=((-0.316735294117647)+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9))+x1319);
evalcond[3]=((-0.2125)+(((-1.1)*x1317))+(((-1.0)*(1.0)*pp))+(((0.09)*x1314))+(((-0.09)*x1316))+(((-1.1)*x1318)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1320=(cj9*px);
IkReal x1321=(cj9*py);
IkReal x1322=(pp*px);
IkReal x1323=(pp*py);
CheckValue<IkReal> x1324=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1324.valid){
continue;
}
CheckValue<IkReal> x1325 = IKatan2WithCheck(IkReal(((((-0.108264705882353)*x1321))+(((-1.0)*(1.51009803921569)*px))+(((-1.32323529411765)*x1320))+(((-1.0)*(0.316735294117647)*py))+(((3.92156862745098)*x1322))+(((-0.588235294117647)*x1323)))),((((-3.92156862745098)*x1323))+(((-0.588235294117647)*x1322))+(((-0.108264705882353)*x1320))+(((1.51009803921569)*py))+(((1.32323529411765)*x1321))+(((-1.0)*(0.316735294117647)*px))),IKFAST_ATAN2_MAGTHRESH);
if(!x1325.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1324.value)))+(x1325.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1326=IKsin(j4);
IkReal x1327=(px*x1326);
IkReal x1328=IKcos(j4);
IkReal x1329=(py*x1328);
IkReal x1330=(px*x1328);
IkReal x1331=(py*x1326);
IkReal x1332=((((-1.0)*x1330))+(((-1.0)*x1331)));
evalcond[0]=((1.51009803921569)+x1327+(((-1.0)*x1329))+(((-1.0)*(3.92156862745098)*pp))+(((1.32323529411765)*cj9)));
evalcond[1]=((-0.55)+x1332+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[2]=((-0.316735294117647)+x1332+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9)));
evalcond[3]=((-0.2125)+(((-1.1)*x1330))+(((0.09)*x1327))+(((-0.09)*x1329))+(((-1.0)*(1.0)*pp))+(((-1.1)*x1331)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1333=(cj9*px);
IkReal x1334=(cj9*py);
IkReal x1335=((3.92156862745098)*pp);
IkReal x1336=((0.045)*sj9);
CheckValue<IkReal> x1337 = IKatan2WithCheck(IkReal(((((-1.32323529411765)*x1333))+(((-0.3)*x1334))+((px*x1335))+(((-1.0)*(1.51009803921569)*px))+(((-1.0)*py*x1336))+(((-1.0)*(0.55)*py)))),((((-1.0)*(0.55)*px))+(((-1.0)*py*x1335))+(((-1.0)*px*x1336))+(((1.51009803921569)*py))+(((-0.3)*x1333))+(((1.32323529411765)*x1334))),IKFAST_ATAN2_MAGTHRESH);
if(!x1337.valid){
continue;
}
CheckValue<IkReal> x1338=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1338.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1337.value)+(((1.5707963267949)*(x1338.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[4];
IkReal x1339=IKsin(j4);
IkReal x1340=(px*x1339);
IkReal x1341=IKcos(j4);
IkReal x1342=(py*x1341);
IkReal x1343=(px*x1341);
IkReal x1344=(py*x1339);
IkReal x1345=((((-1.0)*x1343))+(((-1.0)*x1344)));
evalcond[0]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*x1342))+x1340+(((1.32323529411765)*cj9)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9))+x1345);
evalcond[2]=((-0.316735294117647)+(((-1.0)*(0.588235294117647)*pp))+(((-1.0)*(0.108264705882353)*cj9))+x1345);
evalcond[3]=((-0.2125)+(((-1.1)*x1344))+(((-1.1)*x1343))+(((0.09)*x1340))+(((-0.09)*x1342))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1346=(cj6*pz);
IkReal x1347=((1.32323529411765)*cj9);
IkReal x1348=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((-0.55)+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9))+x1346);
evalcond[2]=((1.51009803921569)+(((-1.0)*x1348))+x1347);
evalcond[3]=(pz*sj6);
evalcond[4]=((-1.51009803921569)+(((-1.0)*x1347))+x1348);
evalcond[5]=((((0.108264705882353)*cj9*sj6))+(((0.588235294117647)*pp*sj6))+(((0.316735294117647)*sj6)));
evalcond[6]=((-0.2125)+(((1.1)*x1346))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1349=((1.32323529411765)*cj9);
IkReal x1350=((3.92156862745098)*pp);
IkReal x1351=((0.316735294117647)*sj6);
IkReal x1352=((0.108264705882353)*cj9*sj6);
IkReal x1353=((0.588235294117647)*pp*sj6);
CheckValue<IkReal> x1354 = IKatan2WithCheck(IkReal((((py*x1353))+(((-1.0)*(1.51009803921569)*px))+(((-1.0)*px*x1349))+((py*x1352))+((px*x1350))+((py*x1351)))),(((px*x1351))+((px*x1353))+(((1.51009803921569)*py))+((py*x1349))+((px*x1352))+(((-1.0)*py*x1350))),IKFAST_ATAN2_MAGTHRESH);
if(!x1354.valid){
continue;
}
CheckValue<IkReal> x1355=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1355.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1354.value)+(((1.5707963267949)*(x1355.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1356=IKcos(j4);
IkReal x1357=((1.0)*x1356);
IkReal x1358=(px*x1357);
IkReal x1359=IKsin(j4);
IkReal x1360=(py*x1359);
IkReal x1361=((1.0)*x1360);
IkReal x1362=(px*x1359);
IkReal x1363=(cj6*pz);
IkReal x1364=(px*sj6*x1356);
IkReal x1365=(sj6*x1360);
evalcond[0]=((((-1.0)*cj6*x1361))+(((-1.0)*cj6*x1358))+((pz*sj6)));
evalcond[1]=((1.51009803921569)+(((-1.0)*py*x1357))+(((-1.0)*(3.92156862745098)*pp))+x1362+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1363+x1365+x1364+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((0.108264705882353)*cj9*sj6))+(((0.588235294117647)*pp*sj6))+(((-1.0)*x1361))+(((-1.0)*x1358))+(((0.316735294117647)*sj6)));
evalcond[4]=((-0.2125)+(((1.1)*x1364))+(((1.1)*x1365))+(((-0.09)*py*x1356))+(((-1.0)*(1.0)*pp))+(((1.1)*x1363))+(((0.09)*x1362)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1366=(cj6*pp);
IkReal x1367=((0.2125)*cj6);
IkReal x1368=((1.1)*pz);
IkReal x1369=((0.09)*pz*sj6);
CheckValue<IkReal> x1370 = IKatan2WithCheck(IkReal((((px*x1368))+(((-1.0)*px*x1367))+(((-1.0)*px*x1366))+(((-1.0)*py*x1369)))),((((-1.0)*px*x1369))+(((-1.0)*py*x1368))+((py*x1367))+((py*x1366))),IKFAST_ATAN2_MAGTHRESH);
if(!x1370.valid){
continue;
}
CheckValue<IkReal> x1371=IKPowWithIntegerCheck(IKsign(((((0.09)*cj6*(pz*pz)))+(((-0.09)*x1366)))),-1);
if(!x1371.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1370.value)+(((1.5707963267949)*(x1371.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1372=IKcos(j4);
IkReal x1373=((1.0)*x1372);
IkReal x1374=(px*x1373);
IkReal x1375=IKsin(j4);
IkReal x1376=(py*x1375);
IkReal x1377=((1.0)*x1376);
IkReal x1378=(px*x1375);
IkReal x1379=(cj6*pz);
IkReal x1380=(px*sj6*x1372);
IkReal x1381=(sj6*x1376);
evalcond[0]=((((-1.0)*cj6*x1374))+((pz*sj6))+(((-1.0)*cj6*x1377)));
evalcond[1]=((1.51009803921569)+x1378+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*py*x1373))+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.55)+x1379+x1380+x1381+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((0.108264705882353)*cj9*sj6))+(((-1.0)*x1374))+(((0.588235294117647)*pp*sj6))+(((0.316735294117647)*sj6))+(((-1.0)*x1377)));
evalcond[4]=((-0.2125)+(((-0.09)*py*x1372))+(((0.09)*x1378))+(((1.1)*x1379))+(((1.1)*x1381))+(((1.1)*x1380))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1382=((1.51009803921569)*cj6);
IkReal x1383=(pz*sj6);
IkReal x1384=((1.32323529411765)*cj6*cj9);
IkReal x1385=((3.92156862745098)*cj6*pp);
CheckValue<IkReal> x1386=IKPowWithIntegerCheck(IKsign(((((-1.0)*(1.0)*cj6*(pz*pz)))+((cj6*pp)))),-1);
if(!x1386.valid){
continue;
}
CheckValue<IkReal> x1387 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1384))+(((-1.0)*px*x1382))+((py*x1383))+((px*x1385)))),((((-1.0)*py*x1385))+((py*x1384))+((px*x1383))+((py*x1382))),IKFAST_ATAN2_MAGTHRESH);
if(!x1387.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1386.value)))+(x1387.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1388=IKcos(j4);
IkReal x1389=((1.0)*x1388);
IkReal x1390=(px*x1389);
IkReal x1391=IKsin(j4);
IkReal x1392=(py*x1391);
IkReal x1393=((1.0)*x1392);
IkReal x1394=(px*x1391);
IkReal x1395=(cj6*pz);
IkReal x1396=(px*sj6*x1388);
IkReal x1397=(sj6*x1392);
evalcond[0]=((((-1.0)*cj6*x1393))+((pz*sj6))+(((-1.0)*cj6*x1390)));
evalcond[1]=((1.51009803921569)+(((-1.0)*py*x1389))+(((-1.0)*(3.92156862745098)*pp))+x1394+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.55)+(((-1.0)*(0.3)*cj9))+x1397+x1396+x1395+(((-1.0)*(0.045)*sj9)));
evalcond[3]=((((-1.0)*x1393))+(((0.108264705882353)*cj9*sj6))+(((0.588235294117647)*pp*sj6))+(((0.316735294117647)*sj6))+(((-1.0)*x1390)));
evalcond[4]=((-0.2125)+(((1.1)*x1395))+(((0.09)*x1394))+(((-0.09)*py*x1388))+(((1.1)*x1396))+(((1.1)*x1397))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1398=((-0.55)+pz+(((-1.0)*(0.3)*cj9))+(((-1.0)*(0.045)*sj9)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j6))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=x1398;
evalcond[3]=x1398;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
IkReal x1399=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1400=((1.51009803921569)*cj8);
IkReal x1401=((1.51009803921569)*sj8);
IkReal x1402=((1.32323529411765)*cj8*cj9);
IkReal x1403=((3.92156862745098)*cj8*pp);
IkReal x1404=((1.32323529411765)*cj9*sj8);
IkReal x1405=((3.92156862745098)*pp*sj8);
j4eval[0]=x1399;
j4eval[1]=((IKabs((((py*x1403))+((px*x1401))+(((-1.0)*py*x1402))+((px*x1404))+(((-1.0)*px*x1405))+(((-1.0)*py*x1400)))))+(IKabs(((((-1.0)*px*x1400))+(((-1.0)*py*x1404))+(((-1.0)*px*x1402))+(((-1.0)*py*x1401))+((py*x1405))+((px*x1403))))));
j4eval[2]=IKsign(x1399);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
sj6=0;
cj6=1.0;
j6=0;
IkReal x1406=(((cj8*(pz*pz)))+(((-1.0)*(1.0)*cj8*pp)));
j4eval[0]=x1406;
j4eval[1]=IKsign(x1406);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
sj6=0;
cj6=1.0;
j6=0;
IkReal x1407=((((-1.0)*(1.0)*sj8*(pz*pz)))+((pp*sj8)));
j4eval[0]=x1407;
j4eval[1]=IKsign(x1407);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j8))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=0;
cj8=1.0;
j8=0;
IkReal x1408=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1409=((13497.0)*cj9);
IkReal x1410=((40000.0)*pp);
j4eval[0]=x1408;
j4eval[1]=((IKabs((((py*x1410))+(((-1.0)*(15403.0)*py))+(((-1.0)*py*x1409)))))+(IKabs(((((-1.0)*px*x1409))+(((-1.0)*(15403.0)*px))+((px*x1410))))));
j4eval[2]=IKsign(x1408);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=0;
cj8=1.0;
j8=0;
IkReal x1411=pz*pz;
IkReal x1412=((80.0)*pp);
IkReal x1413=((88.0)*pz);
j4eval[0]=((((-1.0)*x1411))+pp);
j4eval[1]=((IKabs(((((-1.0)*px*x1413))+((px*x1412))+(((17.0)*px)))))+(IKabs((((py*x1412))+(((17.0)*py))+(((-1.0)*py*x1413))))));
j4eval[2]=IKsign(((((9.0)*pp))+(((-9.0)*x1411))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1414=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=0;
evalcond[2]=x1414;
evalcond[3]=x1414;
evalcond[4]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1415=((100.0)*pp);
IkReal x1416=((110.0)*pz);
CheckValue<IkReal> x1417=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1417.valid){
continue;
}
CheckValue<IkReal> x1418 = IKatan2WithCheck(IkReal((((py*x1415))+(((21.25)*py))+(((-1.0)*py*x1416)))),(((px*x1415))+(((-1.0)*px*x1416))+(((21.25)*px))),IKFAST_ATAN2_MAGTHRESH);
if(!x1418.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1417.value)))+(x1418.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1419=IKsin(j4);
IkReal x1420=IKcos(j4);
IkReal x1421=(px*x1420);
IkReal x1422=(py*x1419);
evalcond[0]=((((-1.0)*py*x1420))+((px*x1419)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((-1.0)*x1421))+(((-1.0)*x1422))+(((3.92156862745098)*pp)));
evalcond[2]=((-0.2125)+(((1.1)*pz))+(((0.09)*x1421))+(((0.09)*x1422))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1423=((1.32323529411765)*cj9);
IkReal x1424=((3.92156862745098)*pp);
CheckValue<IkReal> x1425 = IKatan2WithCheck(IkReal((((py*x1424))+(((-1.0)*(1.51009803921569)*py))+(((-1.0)*py*x1423)))),((((-1.0)*px*x1423))+(((-1.0)*(1.51009803921569)*px))+((px*x1424))),IKFAST_ATAN2_MAGTHRESH);
if(!x1425.valid){
continue;
}
CheckValue<IkReal> x1426=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1426.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1425.value)+(((1.5707963267949)*(x1426.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1427=IKsin(j4);
IkReal x1428=IKcos(j4);
IkReal x1429=(px*x1428);
IkReal x1430=(py*x1427);
evalcond[0]=((((-1.0)*py*x1428))+((px*x1427)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+(((-1.0)*x1430))+(((-1.0)*x1429)));
evalcond[2]=((-0.2125)+(((1.1)*pz))+(((0.09)*x1430))+(((-1.0)*(1.0)*pp))+(((0.09)*x1429)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j8)))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x1431=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1432=((13497.0)*cj9);
IkReal x1433=((40000.0)*pp);
j4eval[0]=x1431;
j4eval[1]=((IKabs(((((15403.0)*px))+((px*x1432))+(((-1.0)*px*x1433)))))+(IKabs(((((15403.0)*py))+((py*x1432))+(((-1.0)*py*x1433))))));
j4eval[2]=IKsign(x1431);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x1434=pz*pz;
IkReal x1435=((80.0)*pp);
IkReal x1436=((88.0)*pz);
j4eval[0]=(pp+(((-1.0)*x1434)));
j4eval[1]=IKsign(((((9.0)*pp))+(((-9.0)*x1434))));
j4eval[2]=((IKabs(((((-1.0)*(17.0)*px))+((px*x1436))+(((-1.0)*px*x1435)))))+(IKabs((((py*x1436))+(((-1.0)*py*x1435))+(((-1.0)*(17.0)*py))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1437=((1.32323529411765)*cj9);
IkReal x1438=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=0;
evalcond[2]=((-1.51009803921569)+x1438+(((-1.0)*x1437)));
evalcond[3]=((1.51009803921569)+(((-1.0)*x1438))+x1437);
evalcond[4]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1439=((100.0)*pp);
IkReal x1440=((110.0)*pz);
CheckValue<IkReal> x1441=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1441.valid){
continue;
}
CheckValue<IkReal> x1442 = IKatan2WithCheck(IkReal((((py*x1440))+(((-1.0)*(21.25)*py))+(((-1.0)*py*x1439)))),(((px*x1440))+(((-1.0)*(21.25)*px))+(((-1.0)*px*x1439))),IKFAST_ATAN2_MAGTHRESH);
if(!x1442.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1441.value)))+(x1442.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1443=IKsin(j4);
IkReal x1444=IKcos(j4);
IkReal x1445=(px*x1444);
IkReal x1446=(py*x1443);
evalcond[0]=((((-1.0)*py*x1444))+((px*x1443)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1446+x1445);
evalcond[2]=((-0.2125)+(((1.1)*pz))+(((-0.09)*x1446))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1445)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1447=((1.32323529411765)*cj9);
IkReal x1448=((3.92156862745098)*pp);
CheckValue<IkReal> x1449=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1449.valid){
continue;
}
CheckValue<IkReal> x1450 = IKatan2WithCheck(IkReal(((((-1.0)*py*x1448))+(((1.51009803921569)*py))+((py*x1447)))),((((-1.0)*px*x1448))+(((1.51009803921569)*px))+((px*x1447))),IKFAST_ATAN2_MAGTHRESH);
if(!x1450.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1449.value)))+(x1450.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1451=IKsin(j4);
IkReal x1452=IKcos(j4);
IkReal x1453=(px*x1452);
IkReal x1454=(py*x1451);
evalcond[0]=(((px*x1451))+(((-1.0)*py*x1452)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1453+x1454);
evalcond[2]=((-0.2125)+(((-0.09)*x1453))+(((1.1)*pz))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1454)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j8)))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1455=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1456=((13497.0)*cj9);
IkReal x1457=((40000.0)*pp);
j4eval[0]=x1455;
j4eval[1]=((IKabs(((((15403.0)*px))+(((-1.0)*px*x1457))+((px*x1456)))))+(IKabs(((((-1.0)*py*x1456))+((py*x1457))+(((-1.0)*(15403.0)*py))))));
j4eval[2]=IKsign(x1455);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1458=pz*pz;
IkReal x1459=((80.0)*pp);
IkReal x1460=((88.0)*pz);
j4eval[0]=(pp+(((-1.0)*x1458)));
j4eval[1]=IKsign(((((-9.0)*x1458))+(((9.0)*pp))));
j4eval[2]=((IKabs(((((-1.0)*py*x1460))+((py*x1459))+(((17.0)*py)))))+(IKabs(((((-1.0)*(17.0)*px))+(((-1.0)*px*x1459))+((px*x1460))))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1461=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=x1461;
evalcond[2]=0;
evalcond[3]=x1461;
evalcond[4]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1462=((100.0)*pp);
IkReal x1463=((110.0)*pz);
CheckValue<IkReal> x1464=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1464.valid){
continue;
}
CheckValue<IkReal> x1465 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1462))+(((-1.0)*(21.25)*px))+((px*x1463)))),(((py*x1462))+(((21.25)*py))+(((-1.0)*py*x1463))),IKFAST_ATAN2_MAGTHRESH);
if(!x1465.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1464.value)))+(x1465.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1466=IKcos(j4);
IkReal x1467=IKsin(j4);
IkReal x1468=(px*x1467);
IkReal x1469=(py*x1466);
evalcond[0]=(((py*x1467))+((px*x1466)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*x1469))+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1468);
evalcond[2]=((-0.2125)+(((1.1)*pz))+(((0.09)*x1469))+(((-0.09)*x1468))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1470=((1.32323529411765)*cj9);
IkReal x1471=((3.92156862745098)*pp);
CheckValue<IkReal> x1472=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1472.valid){
continue;
}
CheckValue<IkReal> x1473 = IKatan2WithCheck(IkReal((((px*x1470))+(((-1.0)*px*x1471))+(((1.51009803921569)*px)))),(((py*x1471))+(((-1.0)*(1.51009803921569)*py))+(((-1.0)*py*x1470))),IKFAST_ATAN2_MAGTHRESH);
if(!x1473.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1472.value)))+(x1473.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1474=IKcos(j4);
IkReal x1475=IKsin(j4);
IkReal x1476=(px*x1475);
IkReal x1477=(py*x1474);
evalcond[0]=(((px*x1474))+((py*x1475)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+(((-1.0)*x1477))+x1476);
evalcond[2]=((-0.2125)+(((-0.09)*x1476))+(((0.09)*x1477))+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j8)))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1478=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1479=((13497.0)*cj9);
IkReal x1480=((40000.0)*pp);
j4eval[0]=x1478;
j4eval[1]=((IKabs(((((-1.0)*py*x1480))+(((15403.0)*py))+((py*x1479)))))+(IKabs(((((-1.0)*px*x1479))+((px*x1480))+(((-1.0)*(15403.0)*px))))));
j4eval[2]=IKsign(x1478);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=1.0;
j6=0;
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1481=pz*pz;
IkReal x1482=((80.0)*pp);
IkReal x1483=((88.0)*pz);
j4eval[0]=((((-1.0)*x1481))+pp);
j4eval[1]=((IKabs(((((-1.0)*py*x1482))+((py*x1483))+(((-1.0)*(17.0)*py)))))+(IKabs(((((17.0)*px))+((px*x1482))+(((-1.0)*px*x1483))))));
j4eval[2]=IKsign(((((-9.0)*x1481))+(((9.0)*pp))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1484=((1.32323529411765)*cj9);
IkReal x1485=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((1.51009803921569)+(((-1.0)*x1485))+x1484);
evalcond[2]=0;
evalcond[3]=((-1.51009803921569)+x1485+(((-1.0)*x1484)));
evalcond[4]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1486=((100.0)*pp);
IkReal x1487=((110.0)*pz);
CheckValue<IkReal> x1488 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1487))+((px*x1486))+(((21.25)*px)))),((((-1.0)*py*x1486))+(((-1.0)*(21.25)*py))+((py*x1487))),IKFAST_ATAN2_MAGTHRESH);
if(!x1488.valid){
continue;
}
CheckValue<IkReal> x1489=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1489.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1488.value)+(((1.5707963267949)*(x1489.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1490=IKcos(j4);
IkReal x1491=((1.0)*x1490);
IkReal x1492=IKsin(j4);
IkReal x1493=(px*x1492);
evalcond[0]=((((-1.0)*py*x1492))+(((-1.0)*px*x1491)));
evalcond[1]=((1.51009803921569)+x1493+(((-1.0)*py*x1491))+(((-1.0)*(3.92156862745098)*pp))+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp))+(((-0.09)*py*x1490))+(((0.09)*x1493)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1494=((1.32323529411765)*cj9);
IkReal x1495=((3.92156862745098)*pp);
CheckValue<IkReal> x1496 = IKatan2WithCheck(IkReal(((((-1.0)*(1.51009803921569)*px))+(((-1.0)*px*x1494))+((px*x1495)))),((((-1.0)*py*x1495))+(((1.51009803921569)*py))+((py*x1494))),IKFAST_ATAN2_MAGTHRESH);
if(!x1496.valid){
continue;
}
CheckValue<IkReal> x1497=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1497.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1496.value)+(((1.5707963267949)*(x1497.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1498=IKcos(j4);
IkReal x1499=((1.0)*x1498);
IkReal x1500=IKsin(j4);
IkReal x1501=(px*x1500);
evalcond[0]=((((-1.0)*px*x1499))+(((-1.0)*py*x1500)));
evalcond[1]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*py*x1499))+x1501+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.2125)+(((-0.09)*py*x1498))+(((1.1)*pz))+(((-1.0)*(1.0)*pp))+(((0.09)*x1501)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1502=((1.32323529411765)*cj9);
IkReal x1503=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=(((sj8*x1503))+(((-1.0)*(1.51009803921569)*sj8))+(((-1.0)*sj8*x1502)));
evalcond[2]=0;
evalcond[3]=((-1.51009803921569)+(((-1.0)*x1502))+x1503);
evalcond[4]=((((-1.0)*(1.51009803921569)*cj8))+((cj8*x1503))+(((-1.0)*cj8*x1502)));
evalcond[5]=((-0.2125)+(((1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1504=((1.51009803921569)*px);
IkReal x1505=((1.32323529411765)*cj9);
IkReal x1506=(px*x1505);
IkReal x1507=((3.92156862745098)*pp);
IkReal x1508=(px*x1507);
IkReal x1509=((1.51009803921569)*py);
IkReal x1510=(cj8*sj8);
IkReal x1511=cj8*cj8;
IkReal x1512=(py*x1505);
IkReal x1513=((3.92156862745098)*cj8*pp*sj8);
IkReal x1514=(py*x1507);
CheckValue<IkReal> x1515=IKPowWithIntegerCheck(IKsign(((((-1.0)*(1.0)*sj8*(pz*pz)))+((pp*sj8)))),-1);
if(!x1515.valid){
continue;
}
CheckValue<IkReal> x1516 = IKatan2WithCheck(IkReal(((((-1.0)*x1506*x1511))+(((-1.0)*x1510*x1512))+(((-1.0)*x1508))+(((-1.0)*x1504*x1511))+((x1508*x1511))+((py*x1513))+(((-1.0)*x1509*x1510))+x1504+x1506)),((((-1.0)*x1512))+((px*x1513))+(((-1.0)*x1506*x1510))+x1514+(((-1.0)*x1511*x1514))+((x1509*x1511))+(((-1.0)*x1504*x1510))+(((-1.0)*x1509))+((x1511*x1512))),IKFAST_ATAN2_MAGTHRESH);
if(!x1516.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1515.value)))+(x1516.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1517=((1.32323529411765)*cj9);
IkReal x1518=((3.92156862745098)*pp);
IkReal x1519=IKsin(j4);
IkReal x1520=(px*x1519);
IkReal x1521=IKcos(j4);
IkReal x1522=((1.0)*x1521);
IkReal x1523=(py*x1522);
IkReal x1524=(px*x1522);
IkReal x1525=(py*x1519);
IkReal x1526=((1.0)*x1525);
IkReal x1527=(px*x1521);
IkReal x1528=(sj8*x1520);
IkReal x1529=((0.09)*cj8);
evalcond[0]=((((-1.0)*sj8*x1517))+(((-1.0)*x1523))+((sj8*x1518))+x1520+(((-1.0)*(1.51009803921569)*sj8)));
evalcond[1]=((((-1.0)*(1.51009803921569)*cj8))+(((-1.0)*x1526))+((cj8*x1518))+(((-1.0)*x1524))+(((-1.0)*cj8*x1517)));
evalcond[2]=(((sj8*x1527))+(((-1.0)*cj8*x1523))+((cj8*x1520))+((sj8*x1525)));
evalcond[3]=((-1.51009803921569)+x1518+(((-1.0)*cj8*x1526))+(((-1.0)*sj8*x1523))+(((-1.0)*x1517))+(((-1.0)*cj8*x1524))+x1528);
evalcond[4]=((-0.2125)+((x1527*x1529))+(((0.09)*py*sj8*x1521))+((x1525*x1529))+(((1.1)*pz))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1528)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1530=((1.51009803921569)*cj8*sj8);
IkReal x1531=cj8*cj8;
IkReal x1532=((1.51009803921569)*x1531);
IkReal x1533=((1.32323529411765)*cj8*cj9*sj8);
IkReal x1534=((3.92156862745098)*cj8*pp*sj8);
IkReal x1535=((1.32323529411765)*cj9*x1531);
IkReal x1536=((3.92156862745098)*pp*x1531);
CheckValue<IkReal> x1537 = IKatan2WithCheck(IkReal(((((-1.0)*py*x1536))+((px*x1534))+(((-1.0)*px*x1530))+(((-1.0)*px*x1533))+((py*x1535))+((py*x1532)))),(((px*x1532))+(((-1.0)*py*x1534))+(((-1.0)*px*x1536))+((px*x1535))+((py*x1533))+((py*x1530))),IKFAST_ATAN2_MAGTHRESH);
if(!x1537.valid){
continue;
}
CheckValue<IkReal> x1538=IKPowWithIntegerCheck(IKsign((((cj8*(pz*pz)))+(((-1.0)*cj8*pp)))),-1);
if(!x1538.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1537.value)+(((1.5707963267949)*(x1538.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1539=((1.32323529411765)*cj9);
IkReal x1540=((3.92156862745098)*pp);
IkReal x1541=IKsin(j4);
IkReal x1542=(px*x1541);
IkReal x1543=IKcos(j4);
IkReal x1544=((1.0)*x1543);
IkReal x1545=(py*x1544);
IkReal x1546=(px*x1544);
IkReal x1547=(py*x1541);
IkReal x1548=((1.0)*x1547);
IkReal x1549=(px*x1543);
IkReal x1550=(sj8*x1542);
IkReal x1551=((0.09)*cj8);
evalcond[0]=(x1542+(((-1.0)*sj8*x1539))+((sj8*x1540))+(((-1.0)*x1545))+(((-1.0)*(1.51009803921569)*sj8)));
evalcond[1]=((((-1.0)*(1.51009803921569)*cj8))+((cj8*x1540))+(((-1.0)*x1546))+(((-1.0)*cj8*x1539))+(((-1.0)*x1548)));
evalcond[2]=(((sj8*x1547))+(((-1.0)*cj8*x1545))+((cj8*x1542))+((sj8*x1549)));
evalcond[3]=((-1.51009803921569)+(((-1.0)*cj8*x1546))+x1540+x1550+(((-1.0)*x1539))+(((-1.0)*cj8*x1548))+(((-1.0)*sj8*x1545)));
evalcond[4]=((-0.2125)+(((1.1)*pz))+((x1549*x1551))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1550))+(((0.09)*py*sj8*x1543))+((x1547*x1551)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1552=((1.51009803921569)*cj8);
IkReal x1553=((1.51009803921569)*sj8);
IkReal x1554=((1.32323529411765)*cj8*cj9);
IkReal x1555=((3.92156862745098)*cj8*pp);
IkReal x1556=((1.32323529411765)*cj9*sj8);
IkReal x1557=((3.92156862745098)*pp*sj8);
CheckValue<IkReal> x1558 = IKatan2WithCheck(IkReal(((((-1.0)*py*x1554))+(((-1.0)*py*x1552))+(((-1.0)*px*x1557))+((py*x1555))+((px*x1556))+((px*x1553)))),((((-1.0)*px*x1552))+((py*x1557))+(((-1.0)*py*x1553))+(((-1.0)*py*x1556))+(((-1.0)*px*x1554))+((px*x1555))),IKFAST_ATAN2_MAGTHRESH);
if(!x1558.valid){
continue;
}
CheckValue<IkReal> x1559=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1559.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1558.value)+(((1.5707963267949)*(x1559.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1560=((1.32323529411765)*cj9);
IkReal x1561=((3.92156862745098)*pp);
IkReal x1562=IKsin(j4);
IkReal x1563=(px*x1562);
IkReal x1564=IKcos(j4);
IkReal x1565=((1.0)*x1564);
IkReal x1566=(py*x1565);
IkReal x1567=(px*x1565);
IkReal x1568=(py*x1562);
IkReal x1569=((1.0)*x1568);
IkReal x1570=(px*x1564);
IkReal x1571=(sj8*x1563);
IkReal x1572=((0.09)*cj8);
evalcond[0]=(((sj8*x1561))+x1563+(((-1.0)*(1.51009803921569)*sj8))+(((-1.0)*sj8*x1560))+(((-1.0)*x1566)));
evalcond[1]=((((-1.0)*(1.51009803921569)*cj8))+(((-1.0)*cj8*x1560))+(((-1.0)*x1569))+((cj8*x1561))+(((-1.0)*x1567)));
evalcond[2]=((((-1.0)*cj8*x1566))+((sj8*x1570))+((cj8*x1563))+((sj8*x1568)));
evalcond[3]=((-1.51009803921569)+(((-1.0)*x1560))+(((-1.0)*sj8*x1566))+x1571+x1561+(((-1.0)*cj8*x1569))+(((-1.0)*cj8*x1567)));
evalcond[4]=((-0.2125)+(((-0.09)*x1571))+((x1570*x1572))+(((0.09)*py*sj8*x1564))+(((1.1)*pz))+(((-1.0)*(1.0)*pp))+((x1568*x1572)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1573=((0.3)*cj9);
IkReal x1574=((0.045)*sj9);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j6)))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=((-0.55)+(((-1.0)*(1.0)*pz))+(((-1.0)*x1573))+(((-1.0)*x1574)));
evalcond[3]=((0.55)+x1574+x1573+pz);
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
IkReal x1575=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1576=((1.51009803921569)*cj8);
IkReal x1577=((1.51009803921569)*sj8);
IkReal x1578=((1.32323529411765)*cj8*cj9);
IkReal x1579=((3.92156862745098)*cj8*pp);
IkReal x1580=((1.32323529411765)*cj9*sj8);
IkReal x1581=((3.92156862745098)*pp*sj8);
j4eval[0]=x1575;
j4eval[1]=((IKabs(((((-1.0)*py*x1579))+((px*x1577))+((py*x1578))+(((-1.0)*px*x1581))+((py*x1576))+((px*x1580)))))+(IKabs((((px*x1578))+(((-1.0)*py*x1580))+((px*x1576))+(((-1.0)*px*x1579))+((py*x1581))+(((-1.0)*py*x1577))))));
j4eval[2]=IKsign(x1575);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
IkReal x1582=(((cj8*(pz*pz)))+(((-1.0)*(1.0)*cj8*pp)));
j4eval[0]=x1582;
j4eval[1]=IKsign(x1582);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal j4eval[2];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
IkReal x1583=((((-1.0)*(1.0)*cj8*(pz*pz)))+((cj8*pp)));
j4eval[0]=x1583;
j4eval[1]=IKsign(x1583);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j8)))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1584=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1585=((13497.0)*cj9);
IkReal x1586=((40000.0)*pp);
j4eval[0]=x1584;
j4eval[1]=((IKabs(((((15403.0)*px))+((px*x1585))+(((-1.0)*px*x1586)))))+(IKabs(((((-1.0)*py*x1585))+((py*x1586))+(((-1.0)*(15403.0)*py))))));
j4eval[2]=IKsign(x1584);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
sj8=1.0;
cj8=0;
j8=1.5707963267949;
IkReal x1587=pz*pz;
IkReal x1588=((80.0)*pp);
IkReal x1589=((88.0)*pz);
j4eval[0]=(pp+(((-1.0)*x1587)));
j4eval[1]=((IKabs((((py*x1589))+(((17.0)*py))+((py*x1588)))))+(IKabs((((px*x1588))+(((17.0)*px))+((px*x1589))))));
j4eval[2]=IKsign(((((9.0)*pp))+(((-9.0)*x1587))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1590=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp)));
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=x1590;
evalcond[2]=0;
evalcond[3]=x1590;
evalcond[4]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1591=((100.0)*pp);
IkReal x1592=((110.0)*pz);
CheckValue<IkReal> x1593 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1591))+(((-1.0)*(21.25)*px))+(((-1.0)*px*x1592)))),(((py*x1591))+((py*x1592))+(((21.25)*py))),IKFAST_ATAN2_MAGTHRESH);
if(!x1593.valid){
continue;
}
CheckValue<IkReal> x1594=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1594.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1593.value)+(((1.5707963267949)*(x1594.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1595=IKcos(j4);
IkReal x1596=((1.0)*x1595);
IkReal x1597=IKsin(j4);
IkReal x1598=(px*x1597);
evalcond[0]=((((-1.0)*px*x1596))+(((-1.0)*py*x1597)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*py*x1596))+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1598);
evalcond[2]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-0.09)*x1598))+(((0.09)*py*x1595))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1599=((1.32323529411765)*cj9);
IkReal x1600=((3.92156862745098)*pp);
CheckValue<IkReal> x1601=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1601.valid){
continue;
}
CheckValue<IkReal> x1602 = IKatan2WithCheck(IkReal((((px*x1599))+(((1.51009803921569)*px))+(((-1.0)*px*x1600)))),((((-1.0)*(1.51009803921569)*py))+(((-1.0)*py*x1599))+((py*x1600))),IKFAST_ATAN2_MAGTHRESH);
if(!x1602.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1601.value)))+(x1602.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1603=IKcos(j4);
IkReal x1604=((1.0)*x1603);
IkReal x1605=IKsin(j4);
IkReal x1606=(px*x1605);
evalcond[0]=((((-1.0)*px*x1604))+(((-1.0)*py*x1605)));
evalcond[1]=((-1.51009803921569)+(((-1.0)*(1.32323529411765)*cj9))+(((3.92156862745098)*pp))+x1606+(((-1.0)*py*x1604)));
evalcond[2]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-0.09)*x1606))+(((-1.0)*(1.0)*pp))+(((0.09)*py*x1603)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j8)))), 6.28318530717959)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4eval[3];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1607=(pp+(((-1.0)*(1.0)*(pz*pz))));
IkReal x1608=((13497.0)*cj9);
IkReal x1609=((40000.0)*pp);
j4eval[0]=x1607;
j4eval[1]=((IKabs(((((-1.0)*py*x1609))+(((15403.0)*py))+((py*x1608)))))+(IKabs(((((-1.0)*px*x1608))+(((-1.0)*(15403.0)*px))+((px*x1609))))));
j4eval[2]=IKsign(x1607);
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal j4eval[3];
sj6=0;
cj6=-1.0;
j6=3.14159265358979;
sj8=-1.0;
cj8=0;
j8=-1.5707963267949;
IkReal x1610=pz*pz;
IkReal x1611=((80.0)*pp);
IkReal x1612=((88.0)*pz);
j4eval[0]=(pp+(((-1.0)*x1610)));
j4eval[1]=((IKabs(((((17.0)*py))+((py*x1612))+((py*x1611)))))+(IKabs(((((17.0)*px))+((px*x1612))+((px*x1611))))));
j4eval[2]=IKsign(((((-9.0)*x1610))+(((9.0)*pp))));
if( IKabs(j4eval[0]) < 0.0000010000000000  || IKabs(j4eval[1]) < 0.0000010000000000  || IKabs(j4eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[5];
bool bgotonextstatement = true;
do
{
IkReal x1613=((1.32323529411765)*cj9);
IkReal x1614=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((1.51009803921569)+(((-1.0)*x1614))+x1613);
evalcond[2]=0;
evalcond[3]=((-1.51009803921569)+(((-1.0)*x1613))+x1614);
evalcond[4]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1615=((100.0)*pp);
IkReal x1616=((110.0)*pz);
CheckValue<IkReal> x1617 = IKatan2WithCheck(IkReal((((px*x1616))+(((21.25)*px))+((px*x1615)))),((((-1.0)*py*x1616))+(((-1.0)*(21.25)*py))+(((-1.0)*py*x1615))),IKFAST_ATAN2_MAGTHRESH);
if(!x1617.valid){
continue;
}
CheckValue<IkReal> x1618=IKPowWithIntegerCheck(IKsign(((((-1.0)*(9.0)*(pz*pz)))+(((9.0)*pp)))),-1);
if(!x1618.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1617.value)+(((1.5707963267949)*(x1618.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1619=IKcos(j4);
IkReal x1620=IKsin(j4);
IkReal x1621=(px*x1620);
IkReal x1622=(py*x1619);
evalcond[0]=(((px*x1619))+((py*x1620)));
evalcond[1]=((1.51009803921569)+(((-1.0)*x1622))+(((-1.0)*(3.92156862745098)*pp))+x1621+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((0.09)*x1621))+(((-0.09)*x1622))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1623=((1.32323529411765)*cj9);
IkReal x1624=((3.92156862745098)*pp);
CheckValue<IkReal> x1625 = IKatan2WithCheck(IkReal(((((-1.0)*(1.51009803921569)*px))+(((-1.0)*px*x1623))+((px*x1624)))),(((py*x1623))+(((1.51009803921569)*py))+(((-1.0)*py*x1624))),IKFAST_ATAN2_MAGTHRESH);
if(!x1625.valid){
continue;
}
CheckValue<IkReal> x1626=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1626.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1625.value)+(((1.5707963267949)*(x1626.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[3];
IkReal x1627=IKcos(j4);
IkReal x1628=IKsin(j4);
IkReal x1629=(px*x1628);
IkReal x1630=(py*x1627);
evalcond[0]=(((py*x1628))+((px*x1627)));
evalcond[1]=((1.51009803921569)+(((-1.0)*(3.92156862745098)*pp))+(((-1.0)*x1630))+x1629+(((1.32323529411765)*cj9)));
evalcond[2]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((0.09)*x1629))+(((-0.09)*x1630))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x1631=((1.32323529411765)*cj9);
IkReal x1632=((3.92156862745098)*pp);
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=((((-1.0)*sj8*x1631))+((sj8*x1632))+(((-1.0)*(1.51009803921569)*sj8)));
evalcond[2]=0;
evalcond[3]=((-1.51009803921569)+(((-1.0)*x1631))+x1632);
evalcond[4]=((((1.51009803921569)*cj8))+(((-1.0)*cj8*x1632))+((cj8*x1631)));
evalcond[5]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j4array[4], cj4array[4], sj4array[4];
bool j4valid[4]={false};
_nj4 = 4;
j4array[0]=0;
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=1.5707963267949;
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
j4array[2]=3.14159265358979;
sj4array[2]=IKsin(j4array[2]);
cj4array[2]=IKcos(j4array[2]);
j4array[3]=-1.5707963267949;
sj4array[3]=IKsin(j4array[3]);
cj4array[3]=IKcos(j4array[3]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
if( j4array[2] > IKPI )
{
    j4array[2]-=IK2PI;
}
else if( j4array[2] < -IKPI )
{    j4array[2]+=IK2PI;
}
j4valid[2] = true;
if( j4array[3] > IKPI )
{
    j4array[3]-=IK2PI;
}
else if( j4array[3] < -IKPI )
{    j4array[3]+=IK2PI;
}
j4valid[3] = true;
for(int ij4 = 0; ij4 < 4; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 4; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1633=((1.51009803921569)*cj8*sj8);
IkReal x1634=cj8*cj8;
IkReal x1635=((1.51009803921569)*x1634);
IkReal x1636=((1.32323529411765)*cj8*cj9*sj8);
IkReal x1637=((3.92156862745098)*cj8*pp*sj8);
IkReal x1638=((1.32323529411765)*cj9*x1634);
IkReal x1639=((3.92156862745098)*pp*x1634);
CheckValue<IkReal> x1640 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1637))+((py*x1635))+((py*x1638))+((px*x1633))+((px*x1636))+(((-1.0)*py*x1639)))),(((px*x1638))+((py*x1637))+(((-1.0)*px*x1639))+(((-1.0)*py*x1633))+((px*x1635))+(((-1.0)*py*x1636))),IKFAST_ATAN2_MAGTHRESH);
if(!x1640.valid){
continue;
}
CheckValue<IkReal> x1641=IKPowWithIntegerCheck(IKsign(((((-1.0)*(1.0)*cj8*(pz*pz)))+((cj8*pp)))),-1);
if(!x1641.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1640.value)+(((1.5707963267949)*(x1641.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1642=((1.32323529411765)*cj9);
IkReal x1643=((3.92156862745098)*pp);
IkReal x1644=IKsin(j4);
IkReal x1645=(px*x1644);
IkReal x1646=IKcos(j4);
IkReal x1647=((1.0)*x1646);
IkReal x1648=(py*x1647);
IkReal x1649=(px*x1647);
IkReal x1650=(py*x1644);
IkReal x1651=((1.0)*x1650);
IkReal x1652=(cj8*px*x1646);
IkReal x1653=(cj8*x1650);
IkReal x1654=(sj8*x1645);
evalcond[0]=(((sj8*x1643))+(((-1.0)*sj8*x1642))+(((-1.0)*x1648))+(((-1.0)*(1.51009803921569)*sj8))+x1645);
evalcond[1]=(((cj8*x1642))+(((1.51009803921569)*cj8))+(((-1.0)*cj8*x1643))+(((-1.0)*x1651))+(((-1.0)*x1649)));
evalcond[2]=((((-1.0)*sj8*x1649))+(((-1.0)*sj8*x1651))+((cj8*x1645))+(((-1.0)*cj8*x1648)));
evalcond[3]=((-1.51009803921569)+(((-1.0)*sj8*x1648))+x1654+x1652+x1653+(((-1.0)*x1642))+x1643);
evalcond[4]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-0.09)*x1654))+(((0.09)*py*sj8*x1646))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1652))+(((-0.09)*x1653)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1655=((1.51009803921569)*cj8*sj8);
IkReal x1656=cj8*cj8;
IkReal x1657=((1.51009803921569)*x1656);
IkReal x1658=((1.32323529411765)*cj8*cj9*sj8);
IkReal x1659=((3.92156862745098)*cj8*pp*sj8);
IkReal x1660=((1.32323529411765)*cj9*x1656);
IkReal x1661=((3.92156862745098)*pp*x1656);
CheckValue<IkReal> x1662=IKPowWithIntegerCheck(IKsign((((cj8*(pz*pz)))+(((-1.0)*cj8*pp)))),-1);
if(!x1662.valid){
continue;
}
CheckValue<IkReal> x1663 = IKatan2WithCheck(IkReal(((((-1.0)*px*x1658))+((py*x1661))+((px*x1659))+(((-1.0)*py*x1660))+(((-1.0)*px*x1655))+(((-1.0)*py*x1657)))),((((-1.0)*px*x1660))+((py*x1658))+(((-1.0)*py*x1659))+((px*x1661))+(((-1.0)*px*x1657))+((py*x1655))),IKFAST_ATAN2_MAGTHRESH);
if(!x1663.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1662.value)))+(x1663.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1664=((1.32323529411765)*cj9);
IkReal x1665=((3.92156862745098)*pp);
IkReal x1666=IKsin(j4);
IkReal x1667=(px*x1666);
IkReal x1668=IKcos(j4);
IkReal x1669=((1.0)*x1668);
IkReal x1670=(py*x1669);
IkReal x1671=(px*x1669);
IkReal x1672=(py*x1666);
IkReal x1673=((1.0)*x1672);
IkReal x1674=(cj8*px*x1668);
IkReal x1675=(cj8*x1672);
IkReal x1676=(sj8*x1667);
evalcond[0]=(((sj8*x1665))+(((-1.0)*sj8*x1664))+(((-1.0)*x1670))+(((-1.0)*(1.51009803921569)*sj8))+x1667);
evalcond[1]=((((-1.0)*cj8*x1665))+(((1.51009803921569)*cj8))+(((-1.0)*x1673))+((cj8*x1664))+(((-1.0)*x1671)));
evalcond[2]=(((cj8*x1667))+(((-1.0)*sj8*x1673))+(((-1.0)*cj8*x1670))+(((-1.0)*sj8*x1671)));
evalcond[3]=((-1.51009803921569)+x1676+x1674+x1675+(((-1.0)*x1664))+(((-1.0)*sj8*x1670))+x1665);
evalcond[4]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((0.09)*py*sj8*x1668))+(((-0.09)*x1674))+(((-0.09)*x1676))+(((-1.0)*(1.0)*pp))+(((-0.09)*x1675)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1677=((1.51009803921569)*cj8);
IkReal x1678=((1.51009803921569)*sj8);
IkReal x1679=((1.32323529411765)*cj8*cj9);
IkReal x1680=((3.92156862745098)*cj8*pp);
IkReal x1681=((1.32323529411765)*cj9*sj8);
IkReal x1682=((3.92156862745098)*pp*sj8);
CheckValue<IkReal> x1683=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1683.valid){
continue;
}
CheckValue<IkReal> x1684 = IKatan2WithCheck(IkReal((((py*x1677))+(((-1.0)*px*x1682))+((px*x1678))+((py*x1679))+((px*x1681))+(((-1.0)*py*x1680)))),((((-1.0)*py*x1678))+((py*x1682))+(((-1.0)*px*x1680))+((px*x1679))+((px*x1677))+(((-1.0)*py*x1681))),IKFAST_ATAN2_MAGTHRESH);
if(!x1684.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1683.value)))+(x1684.value));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[5];
IkReal x1685=((1.32323529411765)*cj9);
IkReal x1686=((3.92156862745098)*pp);
IkReal x1687=IKsin(j4);
IkReal x1688=(px*x1687);
IkReal x1689=IKcos(j4);
IkReal x1690=((1.0)*x1689);
IkReal x1691=(py*x1690);
IkReal x1692=(px*x1690);
IkReal x1693=(py*x1687);
IkReal x1694=((1.0)*x1693);
IkReal x1695=(cj8*px*x1689);
IkReal x1696=(cj8*x1693);
IkReal x1697=(sj8*x1688);
evalcond[0]=(x1688+(((-1.0)*x1691))+(((-1.0)*sj8*x1685))+((sj8*x1686))+(((-1.0)*(1.51009803921569)*sj8)));
evalcond[1]=(((cj8*x1685))+(((-1.0)*cj8*x1686))+(((-1.0)*x1692))+(((1.51009803921569)*cj8))+(((-1.0)*x1694)));
evalcond[2]=((((-1.0)*cj8*x1691))+(((-1.0)*sj8*x1692))+((cj8*x1688))+(((-1.0)*sj8*x1694)));
evalcond[3]=((-1.51009803921569)+x1695+x1696+x1697+x1686+(((-1.0)*sj8*x1691))+(((-1.0)*x1685)));
evalcond[4]=((-0.2125)+(((-1.0)*(1.1)*pz))+(((-0.09)*x1697))+(((-0.09)*x1696))+(((-0.09)*x1695))+(((0.09)*py*sj8*x1689))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j4]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1698=((0.045)*sj8);
IkReal x1699=(px*x1698);
IkReal x1700=((0.55)*sj6);
IkReal x1701=((0.045)*cj6*cj8);
IkReal x1702=(py*x1701);
IkReal x1703=((0.3)*cj9*sj6);
IkReal x1704=((0.3)*sj8);
IkReal x1705=(px*sj9);
IkReal x1706=((0.045)*sj6);
IkReal x1707=(py*sj9);
IkReal x1708=((0.3)*cj6*cj8);
IkReal x1709=(py*x1698);
IkReal x1710=(px*x1701);
CheckValue<IkReal> x1711 = IKatan2WithCheck(IkReal(((((-1.0)*x1699))+(((-1.0)*x1704*x1705))+(((-1.0)*cj9*x1702))+((py*x1700))+x1702+((x1706*x1707))+((py*x1703))+((cj9*x1699))+((x1707*x1708)))),(x1709+(((-1.0)*cj9*x1709))+x1710+((px*x1700))+((px*x1703))+((x1705*x1708))+((x1704*x1707))+(((-1.0)*cj9*x1710))+((x1705*x1706))),IKFAST_ATAN2_MAGTHRESH);
if(!x1711.valid){
continue;
}
CheckValue<IkReal> x1712=IKPowWithIntegerCheck(IKsign((pp+(((-1.0)*(1.0)*(pz*pz))))),-1);
if(!x1712.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1711.value)+(((1.5707963267949)*(x1712.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[6];
IkReal x1713=((0.3)*cj9);
IkReal x1714=((0.045)*sj9);
IkReal x1715=(cj6*pz);
IkReal x1716=IKcos(j4);
IkReal x1717=(px*sj6*x1716);
IkReal x1718=IKsin(j4);
IkReal x1719=(py*x1718);
IkReal x1720=(sj6*x1719);
IkReal x1721=((0.045)*cj9);
IkReal x1722=(px*x1718);
IkReal x1723=((0.3)*sj9);
IkReal x1724=((1.0)*x1716);
IkReal x1725=(py*x1724);
IkReal x1726=(sj8*x1716);
IkReal x1727=(cj6*cj8);
IkReal x1728=(px*x1724);
IkReal x1729=((1.0)*x1719);
IkReal x1730=(cj8*pz*sj6);
IkReal x1731=(sj8*x1722);
IkReal x1732=((0.09)*cj6*cj8);
evalcond[0]=((-0.55)+(((-1.0)*x1714))+x1715+x1717+(((-1.0)*x1713))+x1720);
evalcond[1]=((((-1.0)*sj8*x1721))+(((-1.0)*x1725))+(((0.045)*sj8))+x1722+((sj8*x1723)));
evalcond[2]=(((cj6*sj8*x1719))+((cj6*px*x1726))+(((-1.0)*(1.0)*pz*sj6*sj8))+(((-1.0)*cj8*x1725))+((cj8*x1722)));
evalcond[3]=((((0.045)*x1727))+((x1723*x1727))+((sj6*x1714))+(((0.55)*sj6))+(((-1.0)*x1721*x1727))+(((-1.0)*x1729))+(((-1.0)*x1728))+((sj6*x1713)));
evalcond[4]=((0.045)+(((-1.0)*x1721))+(((-1.0)*x1727*x1729))+(((-1.0)*x1727*x1728))+(((-1.0)*sj8*x1725))+x1723+x1731+x1730);
evalcond[5]=((-0.2125)+(((0.09)*py*x1726))+(((-0.09)*x1731))+((x1719*x1732))+(((1.1)*x1717))+((px*x1716*x1732))+(((1.1)*x1720))+(((-1.0)*(1.0)*pp))+(((1.1)*x1715))+(((-0.09)*x1730)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1733=(cj8*sj6);
IkReal x1734=((0.55)*cj8);
IkReal x1735=((0.55)*cj6);
IkReal x1736=(px*sj8);
IkReal x1737=((0.3)*cj8*cj9);
IkReal x1738=((0.045)*cj8*sj9);
IkReal x1739=(cj6*cj8*pz);
IkReal x1740=((0.3)*cj6*cj9);
IkReal x1741=((0.045)*cj6*sj9);
IkReal x1742=(py*sj8);
CheckValue<IkReal> x1743 = IKatan2WithCheck(IkReal(((((-1.0)*pz*x1736))+((x1735*x1736))+((py*x1739))+(((-1.0)*py*x1738))+((x1736*x1740))+(((-1.0)*py*x1737))+(((-1.0)*py*x1734))+((x1736*x1741)))),((((-1.0)*px*x1738))+((px*x1739))+(((-1.0)*px*x1737))+(((-1.0)*px*x1734))+(((-1.0)*x1740*x1742))+(((-1.0)*x1735*x1742))+(((-1.0)*x1741*x1742))+((pz*x1742))),IKFAST_ATAN2_MAGTHRESH);
if(!x1743.valid){
continue;
}
CheckValue<IkReal> x1744=IKPowWithIntegerCheck(IKsign((((x1733*(pz*pz)))+(((-1.0)*pp*x1733)))),-1);
if(!x1744.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1743.value)+(((1.5707963267949)*(x1744.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[6];
IkReal x1745=((0.3)*cj9);
IkReal x1746=((0.045)*sj9);
IkReal x1747=(cj6*pz);
IkReal x1748=IKcos(j4);
IkReal x1749=(px*sj6*x1748);
IkReal x1750=IKsin(j4);
IkReal x1751=(py*x1750);
IkReal x1752=(sj6*x1751);
IkReal x1753=((0.045)*cj9);
IkReal x1754=(px*x1750);
IkReal x1755=((0.3)*sj9);
IkReal x1756=((1.0)*x1748);
IkReal x1757=(py*x1756);
IkReal x1758=(sj8*x1748);
IkReal x1759=(cj6*cj8);
IkReal x1760=(px*x1756);
IkReal x1761=((1.0)*x1751);
IkReal x1762=(cj8*pz*sj6);
IkReal x1763=(sj8*x1754);
IkReal x1764=((0.09)*cj6*cj8);
evalcond[0]=((-0.55)+(((-1.0)*x1746))+(((-1.0)*x1745))+x1747+x1749+x1752);
evalcond[1]=((((0.045)*sj8))+(((-1.0)*sj8*x1753))+((sj8*x1755))+(((-1.0)*x1757))+x1754);
evalcond[2]=(((cj6*sj8*x1751))+(((-1.0)*cj8*x1757))+((cj8*x1754))+(((-1.0)*(1.0)*pz*sj6*sj8))+((cj6*px*x1758)));
evalcond[3]=((((-1.0)*x1761))+(((-1.0)*x1753*x1759))+(((0.55)*sj6))+((x1755*x1759))+((sj6*x1746))+(((0.045)*x1759))+((sj6*x1745))+(((-1.0)*x1760)));
evalcond[4]=((0.045)+(((-1.0)*sj8*x1757))+(((-1.0)*x1759*x1760))+(((-1.0)*x1759*x1761))+x1762+x1763+(((-1.0)*x1753))+x1755);
evalcond[5]=((-0.2125)+(((-0.09)*x1762))+((px*x1748*x1764))+(((1.1)*x1749))+((x1751*x1764))+(((0.09)*py*x1758))+(((-1.0)*(1.0)*pp))+(((1.1)*x1747))+(((1.1)*x1752))+(((-0.09)*x1763)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j4array[1], cj4array[1], sj4array[1];
bool j4valid[1]={false};
_nj4 = 1;
IkReal x1765=((0.3)*cj9);
IkReal x1766=((0.045)*sj9);
IkReal x1767=((1.0)*cj6*pz);
IkReal x1768=((0.045)*sj6*sj8);
IkReal x1769=(px*x1768);
IkReal x1770=((0.3)*sj6*sj8*sj9);
IkReal x1771=(py*x1768);
CheckValue<IkReal> x1772 = IKatan2WithCheck(IkReal((((py*x1765))+((py*x1766))+((cj9*x1769))+(((0.55)*py))+(((-1.0)*x1769))+(((-1.0)*py*x1767))+(((-1.0)*px*x1770)))),(((px*x1766))+((py*x1770))+(((-1.0)*cj9*x1771))+(((0.55)*px))+(((-1.0)*px*x1767))+((px*x1765))+x1771),IKFAST_ATAN2_MAGTHRESH);
if(!x1772.valid){
continue;
}
CheckValue<IkReal> x1773=IKPowWithIntegerCheck(IKsign(((((-1.0)*(1.0)*sj6*(pz*pz)))+((pp*sj6)))),-1);
if(!x1773.valid){
continue;
}
j4array[0]=((-1.5707963267949)+(x1772.value)+(((1.5707963267949)*(x1773.value))));
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
for(int ij4 = 0; ij4 < 1; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 1; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];
{
IkReal evalcond[6];
IkReal x1774=((0.3)*cj9);
IkReal x1775=((0.045)*sj9);
IkReal x1776=(cj6*pz);
IkReal x1777=IKcos(j4);
IkReal x1778=(px*sj6*x1777);
IkReal x1779=IKsin(j4);
IkReal x1780=(py*x1779);
IkReal x1781=(sj6*x1780);
IkReal x1782=((0.045)*cj9);
IkReal x1783=(px*x1779);
IkReal x1784=((0.3)*sj9);
IkReal x1785=((1.0)*x1777);
IkReal x1786=(py*x1785);
IkReal x1787=(sj8*x1777);
IkReal x1788=(cj6*cj8);
IkReal x1789=(px*x1785);
IkReal x1790=((1.0)*x1780);
IkReal x1791=(cj8*pz*sj6);
IkReal x1792=(sj8*x1783);
IkReal x1793=((0.09)*cj6*cj8);
evalcond[0]=((-0.55)+x1781+(((-1.0)*x1775))+(((-1.0)*x1774))+x1778+x1776);
evalcond[1]=((((0.045)*sj8))+(((-1.0)*x1786))+x1783+(((-1.0)*sj8*x1782))+((sj8*x1784)));
evalcond[2]=((((-1.0)*cj8*x1786))+((cj8*x1783))+((cj6*px*x1787))+(((-1.0)*(1.0)*pz*sj6*sj8))+((cj6*sj8*x1780)));
evalcond[3]=((((-1.0)*x1790))+((x1784*x1788))+(((-1.0)*x1782*x1788))+(((0.55)*sj6))+((sj6*x1775))+(((-1.0)*x1789))+(((0.045)*x1788))+((sj6*x1774)));
evalcond[4]=((0.045)+(((-1.0)*x1782))+(((-1.0)*x1788*x1790))+x1784+(((-1.0)*x1788*x1789))+(((-1.0)*sj8*x1786))+x1792+x1791);
evalcond[5]=((-0.2125)+(((1.1)*x1776))+(((1.1)*x1778))+((x1780*x1793))+(((1.1)*x1781))+(((-1.0)*(1.0)*pp))+(((0.09)*py*x1787))+(((-0.09)*x1792))+((px*x1777*x1793))+(((-0.09)*x1791)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}
}
}

}

}

} else
{
{
IkReal j4array[2], cj4array[2], sj4array[2];
bool j4valid[2]={false};
_nj4 = 2;
CheckValue<IkReal> x1797 = IKatan2WithCheck(IkReal(((-1.0)*(((1.0)*py)))),px,IKFAST_ATAN2_MAGTHRESH);
if(!x1797.valid){
continue;
}
IkReal x1794=((-1.0)*(x1797.value));
IkReal x1795=((0.045)*sj8);
if((((py*py)+(px*px))) < -0.00001)
continue;
CheckValue<IkReal> x1798=IKPowWithIntegerCheck(IKabs(IKsqrt(((py*py)+(px*px)))),-1);
if(!x1798.valid){
continue;
}
if( (((x1798.value)*(((((-1.0)*cj9*x1795))+(((0.3)*sj8*sj9))+x1795)))) < -1-IKFAST_SINCOS_THRESH || (((x1798.value)*(((((-1.0)*cj9*x1795))+(((0.3)*sj8*sj9))+x1795)))) > 1+IKFAST_SINCOS_THRESH )
    continue;
IkReal x1796=IKasin(((x1798.value)*(((((-1.0)*cj9*x1795))+(((0.3)*sj8*sj9))+x1795))));
j4array[0]=((((-1.0)*x1796))+x1794);
sj4array[0]=IKsin(j4array[0]);
cj4array[0]=IKcos(j4array[0]);
j4array[1]=((3.14159265358979)+x1794+x1796);
sj4array[1]=IKsin(j4array[1]);
cj4array[1]=IKcos(j4array[1]);
if( j4array[0] > IKPI )
{
    j4array[0]-=IK2PI;
}
else if( j4array[0] < -IKPI )
{    j4array[0]+=IK2PI;
}
j4valid[0] = true;
if( j4array[1] > IKPI )
{
    j4array[1]-=IK2PI;
}
else if( j4array[1] < -IKPI )
{    j4array[1]+=IK2PI;
}
j4valid[1] = true;
for(int ij4 = 0; ij4 < 2; ++ij4)
{
if( !j4valid[ij4] )
{
    continue;
}
_ij4[0] = ij4; _ij4[1] = -1;
for(int iij4 = ij4+1; iij4 < 2; ++iij4)
{
if( j4valid[iij4] && IKabs(cj4array[ij4]-cj4array[iij4]) < IKFAST_SOLUTION_THRESH && IKabs(sj4array[ij4]-sj4array[iij4]) < IKFAST_SOLUTION_THRESH )
{
    j4valid[iij4]=false; _ij4[1] = iij4; break;
}
}
j4 = j4array[ij4]; cj4 = cj4array[ij4]; sj4 = sj4array[ij4];

{
IkReal j6eval[2];
IkReal x1799=(cj4*px);
IkReal x1800=(cj8*pz);
IkReal x1801=(py*sj4);
IkReal x1802=(cj4*cj9*px);
IkReal x1803=(cj4*px*sj9);
IkReal x1804=(cj8*pz*sj9);
IkReal x1805=(cj9*py*sj4);
IkReal x1806=(py*sj4*sj9);
IkReal x1807=((0.045)*x1800);
j6eval[0]=((((-6.66666666666667)*x1802))+(((-1.0)*x1806))+(((-12.2222222222222)*x1799))+(((-1.0)*x1803))+(((-6.66666666666667)*x1804))+(((-1.0)*x1800))+(((-6.66666666666667)*x1805))+(((-12.2222222222222)*x1801))+((cj9*x1800)));
j6eval[1]=IKsign(((((-0.3)*x1804))+(((-0.3)*x1802))+(((-0.045)*x1806))+(((-0.55)*x1799))+((cj9*x1807))+(((-0.3)*x1805))+(((-1.0)*x1807))+(((-0.55)*x1801))+(((-0.045)*x1803))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x1808=(sj8*(py*py));
IkReal x1809=cj4*cj4;
IkReal x1810=((((-1.0)*x1808*x1809))+((sj8*(pz*pz)))+x1808+((sj8*x1809*(px*px)))+(((2.0)*cj4*px*py*sj4*sj8)));
j6eval[0]=x1810;
j6eval[1]=IKsign(x1810);
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x1811=(pz*sj8);
IkReal x1812=(cj9*pz*sj8);
IkReal x1813=(pz*sj8*sj9);
IkReal x1814=(cj8*sj8);
IkReal x1815=(cj4*px*x1814);
IkReal x1816=(py*sj4*x1814);
IkReal x1817=((1.0)*cj9);
IkReal x1818=(cj4*cj8*px*sj8*sj9);
IkReal x1819=(cj8*py*sj4*sj8*sj9);
IkReal x1820=((0.045)*x1815);
IkReal x1821=((0.045)*x1816);
j6eval[0]=((((-1.0)*x1816*x1817))+(((6.66666666666667)*x1818))+(((-1.0)*x1813))+x1815+x1816+(((-1.0)*x1815*x1817))+(((-6.66666666666667)*x1812))+(((-12.2222222222222)*x1811))+(((6.66666666666667)*x1819)));
j6eval[1]=IKsign(((((-1.0)*cj9*x1821))+x1821+x1820+(((0.3)*x1819))+(((-1.0)*cj9*x1820))+(((-0.045)*x1813))+(((0.3)*x1818))+(((-0.55)*x1811))+(((-0.3)*x1812))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
IkReal x1822=(((px*sj4))+(((-1.0)*(1.0)*cj4*py)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j8))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=x1822;
evalcond[3]=x1822;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x1823=(cj9*pz);
IkReal x1824=(cj4*px);
IkReal x1825=(pp*pz);
IkReal x1826=(py*sj4);
IkReal x1827=(cj4*cj9*px);
IkReal x1828=(cj4*pp*px);
IkReal x1829=(cj9*py*sj4);
IkReal x1830=(pp*py*sj4);
j6eval[0]=((((-36.2220411120167)*x1830))+(((13.9482024812098)*x1826))+(((5.4333061668025)*x1825))+x1823+(((13.9482024812098)*x1824))+(((-36.2220411120167)*x1828))+(((12.2222222222222)*x1827))+(((12.2222222222222)*x1829))+(((2.92556370551481)*pz)));
j6eval[1]=IKsign(((((-3.92156862745098)*x1830))+(((0.316735294117647)*pz))+(((-3.92156862745098)*x1828))+(((1.32323529411765)*x1827))+(((0.588235294117647)*x1825))+(((1.51009803921569)*x1824))+(((1.32323529411765)*x1829))+(((1.51009803921569)*x1826))+(((0.108264705882353)*x1823))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x1831=(cj4*px);
IkReal x1832=(py*sj4);
IkReal x1833=(cj9*pz);
IkReal x1834=(pz*sj9);
IkReal x1835=((1.0)*cj9);
IkReal x1836=(cj4*px*sj9);
IkReal x1837=(py*sj4*sj9);
IkReal x1838=((0.045)*x1831);
IkReal x1839=((0.045)*x1832);
j6eval[0]=((((-1.0)*(12.2222222222222)*pz))+(((-1.0)*x1831*x1835))+(((-1.0)*x1834))+(((-6.66666666666667)*x1833))+x1832+x1831+(((6.66666666666667)*x1837))+(((-1.0)*x1832*x1835))+(((6.66666666666667)*x1836)));
j6eval[1]=IKsign(((((-1.0)*(0.55)*pz))+(((0.3)*x1836))+(((-1.0)*cj9*x1839))+(((-1.0)*cj9*x1838))+(((0.3)*x1837))+(((-0.045)*x1834))+(((-0.3)*x1833))+x1839+x1838));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=1.0;
j8=0;
IkReal x1840=(cj4*px);
IkReal x1841=(cj9*pz);
IkReal x1842=(pp*pz);
IkReal x1843=(py*sj4);
IkReal x1844=(cj4*cj9*px);
IkReal x1845=(cj4*pp*px);
IkReal x1846=(cj9*py*sj4);
IkReal x1847=(pp*py*sj4);
j6eval[0]=((((-5.4333061668025)*x1847))+(((-5.4333061668025)*x1845))+(((-36.2220411120167)*x1842))+(((-2.92556370551481)*x1843))+(((-1.0)*x1846))+(((-1.0)*x1844))+(((13.9482024812098)*pz))+(((-2.92556370551481)*x1840))+(((12.2222222222222)*x1841)));
j6eval[1]=IKsign(((((-0.108264705882353)*x1844))+(((1.51009803921569)*pz))+(((1.32323529411765)*x1841))+(((-0.588235294117647)*x1845))+(((-0.108264705882353)*x1846))+(((-0.316735294117647)*x1843))+(((-0.588235294117647)*x1847))+(((-0.316735294117647)*x1840))+(((-3.92156862745098)*x1842))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[1];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j9), 6.28318530717959)))))+(IKabs(pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x1848=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x1848);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x1848);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x1848);
rxp2_1=(px*r22);
j6eval[0]=(((py*sj4))+((cj4*px)));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[1];
IkReal x1849=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x1849);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x1849);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x1849);
rxp2_1=(px*r22);
j6eval[0]=((-1.0)+(((-1.0)*(1.3840830449827)*(px*px)))+(((-1.0)*(1.3840830449827)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x1850=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x1850);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x1850);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x1850);
rxp2_1=(px*r22);
IkReal x1851=(cj4*px);
IkReal x1852=(py*sj4);
j6eval[0]=(x1852+x1851);
j6eval[1]=((((-1.0)*(1.3840830449827)*cj4*(px*px*px)))+(((-1.0)*x1852))+(((-1.0)*x1851))+(((-1.3840830449827)*x1852*(px*px)))+(((-1.0)*(1.3840830449827)*sj4*(py*py*py)))+(((-1.3840830449827)*x1851*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=-0.2125;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
j6array[0]=2.9927027059803;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=6.13429535957009;
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j4), 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x1853=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x1853);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x1853);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x1853);
rxp2_1=0;
px=0;
j4=0;
sj4=0;
cj4=1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x1854=py*py;
CheckValue<IkReal> x1856 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x1854)))),((-2.83333333333333)+(((3.92156862745098)*x1854))),IKFAST_ATAN2_MAGTHRESH);
if(!x1856.valid){
continue;
}
IkReal x1855=((-1.0)*(x1856.value));
j6array[0]=x1855;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x1855);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(j4, 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x1857=((1.0)*py);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x1857);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x1857);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x1857);
rxp2_1=0;
px=0;
j4=3.14159265358979;
sj4=0;
cj4=-1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x1858=py*py;
CheckValue<IkReal> x1860 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x1858)))),((-2.83333333333333)+(((3.92156862745098)*x1858))),IKFAST_ATAN2_MAGTHRESH);
if(!x1860.valid){
continue;
}
IkReal x1859=((-1.0)*(x1860.value));
j6array[0]=x1859;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x1859);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((1.5707963267949)+j4), 6.28318530717959)))))+(IKabs(py)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x1861=((1.0)*px);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=1.5707963267949;
sj4=1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x1861);
rxp1_2=((-1.0)*r11*x1861);
rxp2_2=((-1.0)*r12*x1861);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x1862=px*px;
CheckValue<IkReal> x1864 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x1862)))),((-2.83333333333333)+(((3.92156862745098)*x1862))),IKFAST_ATAN2_MAGTHRESH);
if(!x1864.valid){
continue;
}
IkReal x1863=((-1.0)*(x1864.value));
j6array[0]=x1863;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x1863);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(((-3.14159265358979)+(IKfmod(((4.71238898038469)+j4), 6.28318530717959))))));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x1865=((1.0)*px);
sj8=0;
cj8=1.0;
j8=0;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=-1.5707963267949;
sj4=-1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x1865);
rxp1_2=((-1.0)*r11*x1865);
rxp2_2=((-1.0)*r12*x1865);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x1866=px*px;
CheckValue<IkReal> x1868 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x1866)))),((-2.83333333333333)+(((3.92156862745098)*x1866))),IKFAST_ATAN2_MAGTHRESH);
if(!x1868.valid){
continue;
}
IkReal x1867=((-1.0)*(x1868.value));
j6array[0]=x1867;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x1867);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1869=(cj4*px);
IkReal x1870=(py*sj4);
IkReal x1871=px*px;
IkReal x1872=py*py;
CheckValue<IkReal> x1873=IKPowWithIntegerCheck(((((20.0)*x1870))+(((20.0)*x1869))),-1);
if(!x1873.valid){
continue;
}
CheckValue<IkReal> x1874=IKPowWithIntegerCheck(((((-1.0)*(11.7647058823529)*cj4*(px*px*px)))+(((-11.7647058823529)*x1869*x1872))+(((-8.5)*x1869))+(((-8.5)*x1870))+(((-11.7647058823529)*x1870*x1871))+(((-1.0)*(11.7647058823529)*sj4*(py*py*py)))),-1);
if(!x1874.valid){
continue;
}
if( IKabs(((17.0)*(x1873.value))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x1874.value)*(((48.1666666666667)+(((-66.6666666666667)*x1872))+(((-66.6666666666667)*x1871)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((17.0)*(x1873.value)))+IKsqr(((x1874.value)*(((48.1666666666667)+(((-66.6666666666667)*x1872))+(((-66.6666666666667)*x1871))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((17.0)*(x1873.value)), ((x1874.value)*(((48.1666666666667)+(((-66.6666666666667)*x1872))+(((-66.6666666666667)*x1871))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1875=IKsin(j6);
IkReal x1876=(cj4*px);
IkReal x1877=(x1875*x1876);
IkReal x1878=(py*sj4);
IkReal x1879=(x1875*x1878);
IkReal x1880=((1.0)*x1876);
IkReal x1881=((1.0)*x1878);
IkReal x1882=IKcos(j6);
IkReal x1883=px*px;
IkReal x1884=((3.92156862745098)*x1875);
IkReal x1885=((0.588235294117647)*x1882);
IkReal x1886=py*py;
IkReal x1887=((0.09)*x1882);
evalcond[0]=((-0.85)+x1879+x1877);
evalcond[1]=((((-1.0)*x1880))+(((0.85)*x1875))+(((-1.0)*x1881)));
evalcond[2]=((((-1.0)*x1881*x1882))+(((-1.0)*x1880*x1882)));
evalcond[3]=((((-0.425)*x1882))+(((-1.0)*x1885*x1886))+((x1884*x1886))+(((-1.0)*x1883*x1885))+((x1883*x1884))+(((-2.83333333333333)*x1875)));
evalcond[4]=((-0.2125)+(((1.1)*x1879))+(((-1.0)*x1883))+(((-1.0)*x1886))+(((1.1)*x1877))+((x1876*x1887))+((x1878*x1887)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1888=(cj4*px);
IkReal x1889=(py*sj4);
IkReal x1890=px*px;
IkReal x1891=py*py;
CheckValue<IkReal> x1892=IKPowWithIntegerCheck(((-7.225)+(((-10.0)*x1891))+(((-10.0)*x1890))),-1);
if(!x1892.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x1888))+(((1.17647058823529)*x1889)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x1892.value)*(((((56.6666666666667)*x1889))+(((56.6666666666667)*x1888))+(((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x1888*x1891))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((-78.4313725490196)*x1889*x1890)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x1888))+(((1.17647058823529)*x1889))))+IKsqr(((x1892.value)*(((((56.6666666666667)*x1889))+(((56.6666666666667)*x1888))+(((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x1888*x1891))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((-78.4313725490196)*x1889*x1890))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x1888))+(((1.17647058823529)*x1889))), ((x1892.value)*(((((56.6666666666667)*x1889))+(((56.6666666666667)*x1888))+(((-1.0)*(78.4313725490196)*sj4*(py*py*py)))+(((-78.4313725490196)*x1888*x1891))+(((-1.0)*(78.4313725490196)*cj4*(px*px*px)))+(((-78.4313725490196)*x1889*x1890))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1893=IKsin(j6);
IkReal x1894=(cj4*px);
IkReal x1895=(x1893*x1894);
IkReal x1896=(py*sj4);
IkReal x1897=(x1893*x1896);
IkReal x1898=((1.0)*x1894);
IkReal x1899=((1.0)*x1896);
IkReal x1900=IKcos(j6);
IkReal x1901=px*px;
IkReal x1902=((3.92156862745098)*x1893);
IkReal x1903=((0.588235294117647)*x1900);
IkReal x1904=py*py;
IkReal x1905=((0.09)*x1900);
evalcond[0]=((-0.85)+x1897+x1895);
evalcond[1]=((((-1.0)*x1898))+(((0.85)*x1893))+(((-1.0)*x1899)));
evalcond[2]=((((-1.0)*x1898*x1900))+(((-1.0)*x1899*x1900)));
evalcond[3]=((((-1.0)*x1901*x1903))+(((-1.0)*x1903*x1904))+((x1902*x1904))+((x1901*x1902))+(((-0.425)*x1900))+(((-2.83333333333333)*x1893)));
evalcond[4]=((-0.2125)+(((-1.0)*x1901))+(((1.1)*x1895))+(((1.1)*x1897))+(((-1.0)*x1904))+((x1894*x1905))+((x1896*x1905)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1906=(cj4*px);
IkReal x1907=(py*sj4);
IkReal x1908=px*px;
IkReal x1909=py*py;
IkReal x1910=((1.29411764705882)*(cj4*cj4));
CheckValue<IkReal> x1911=IKPowWithIntegerCheck(((((0.09)*x1907))+(((0.09)*x1906))),-1);
if(!x1911.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x1907))+(((1.17647058823529)*x1906)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x1911.value)*(((0.2125)+(((-1.0)*x1908*x1910))+((x1909*x1910))+x1908+(((-0.294117647058824)*x1909))+(((-2.58823529411765)*cj4*px*x1907)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x1907))+(((1.17647058823529)*x1906))))+IKsqr(((x1911.value)*(((0.2125)+(((-1.0)*x1908*x1910))+((x1909*x1910))+x1908+(((-0.294117647058824)*x1909))+(((-2.58823529411765)*cj4*px*x1907))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x1907))+(((1.17647058823529)*x1906))), ((x1911.value)*(((0.2125)+(((-1.0)*x1908*x1910))+((x1909*x1910))+x1908+(((-0.294117647058824)*x1909))+(((-2.58823529411765)*cj4*px*x1907))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1912=IKsin(j6);
IkReal x1913=(cj4*px);
IkReal x1914=(x1912*x1913);
IkReal x1915=(py*sj4);
IkReal x1916=(x1912*x1915);
IkReal x1917=((1.0)*x1913);
IkReal x1918=((1.0)*x1915);
IkReal x1919=IKcos(j6);
IkReal x1920=px*px;
IkReal x1921=((3.92156862745098)*x1912);
IkReal x1922=((0.588235294117647)*x1919);
IkReal x1923=py*py;
IkReal x1924=((0.09)*x1919);
evalcond[0]=((-0.85)+x1914+x1916);
evalcond[1]=((((0.85)*x1912))+(((-1.0)*x1918))+(((-1.0)*x1917)));
evalcond[2]=((((-1.0)*x1917*x1919))+(((-1.0)*x1918*x1919)));
evalcond[3]=((((-1.0)*x1920*x1922))+(((-1.0)*x1922*x1923))+((x1921*x1923))+((x1920*x1921))+(((-0.425)*x1919))+(((-2.83333333333333)*x1912)));
evalcond[4]=((-0.2125)+((x1913*x1924))+(((-1.0)*x1923))+(((1.1)*x1916))+((x1915*x1924))+(((1.1)*x1914))+(((-1.0)*x1920)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1925=(cj4*px);
IkReal x1926=(py*sj4);
IkReal x1927=((0.108264705882353)*cj9);
IkReal x1928=((0.588235294117647)*pp);
IkReal x1929=(cj9*pp);
IkReal x1930=(cj9*sj9);
IkReal x1931=(pp*sj9);
IkReal x1932=cj9*cj9;
IkReal x1933=((1.0)*pz);
CheckValue<IkReal> x1934=IKPowWithIntegerCheck(IKsign(((((1.51009803921569)*pz))+(((1.32323529411765)*cj9*pz))+(((-1.0)*(3.92156862745098)*pp*pz))+(((-1.0)*x1926*x1928))+(((-1.0)*x1926*x1927))+(((-1.0)*x1925*x1927))+(((-0.316735294117647)*x1925))+(((-0.316735294117647)*x1926))+(((-1.0)*x1925*x1928)))),-1);
if(!x1934.valid){
continue;
}
CheckValue<IkReal> x1935 = IKatan2WithCheck(IkReal(((-0.174204411764706)+(((-0.00487191176470588)*x1930))+(((-0.0264705882352941)*x1931))+(pz*pz)+(((-0.176470588235294)*x1929))+(((-1.0)*(0.154566176470588)*cj9))+(((-0.0324794117647059)*x1932))+(((-1.0)*(0.323529411764706)*pp))+(((-1.0)*(0.0142530882352941)*sj9)))),((0.830553921568627)+(((-1.17647058823529)*x1929))+(((-0.176470588235294)*x1931))+(((0.396970588235294)*x1932))+(((-1.0)*x1926*x1933))+(((1.18080882352941)*cj9))+(((-1.0)*(2.15686274509804)*pp))+(((-1.0)*x1925*x1933))+(((0.0679544117647059)*sj9))+(((0.0595455882352941)*x1930))),IKFAST_ATAN2_MAGTHRESH);
if(!x1935.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x1934.value)))+(x1935.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1936=((0.3)*cj9);
IkReal x1937=((0.045)*sj9);
IkReal x1938=IKcos(j6);
IkReal x1939=(pz*x1938);
IkReal x1940=IKsin(j6);
IkReal x1941=(cj4*px);
IkReal x1942=(x1940*x1941);
IkReal x1943=(py*sj4);
IkReal x1944=(x1940*x1943);
IkReal x1945=((0.045)*cj9);
IkReal x1946=((0.3)*sj9);
IkReal x1947=(pz*x1940);
IkReal x1948=((1.0)*x1941);
IkReal x1949=((1.0)*x1943);
IkReal x1950=((0.09)*x1938);
evalcond[0]=((-0.55)+x1944+x1942+x1939+(((-1.0)*x1937))+(((-1.0)*x1936)));
evalcond[1]=((0.045)+x1946+x1947+(((-1.0)*x1945))+(((-1.0)*x1938*x1948))+(((-1.0)*x1938*x1949)));
evalcond[2]=((((-1.51009803921569)*x1940))+pz+(((3.92156862745098)*pp*x1940))+(((-0.588235294117647)*pp*x1938))+(((-0.108264705882353)*cj9*x1938))+(((-1.32323529411765)*cj9*x1940))+(((-0.316735294117647)*x1938)));
evalcond[3]=((((-1.0)*x1948))+((x1936*x1940))+(((0.55)*x1940))+(((-1.0)*x1938*x1945))+(((-1.0)*x1949))+(((0.045)*x1938))+((x1937*x1940))+((x1938*x1946)));
evalcond[4]=((-0.2125)+(((1.1)*x1944))+((x1943*x1950))+((x1941*x1950))+(((1.1)*x1942))+(((1.1)*x1939))+(((-0.09)*x1947))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1951=((0.045)*cj4*px);
IkReal x1952=((0.045)*py*sj4);
IkReal x1953=((0.3)*sj9);
IkReal x1954=(cj4*px);
IkReal x1955=(py*sj4);
IkReal x1956=(cj9*sj9);
IkReal x1957=cj9*cj9;
IkReal x1958=((1.0)*pz);
IkReal x1959=py*py;
IkReal x1960=cj4*cj4;
CheckValue<IkReal> x1961 = IKatan2WithCheck(IkReal(((0.03825)+(((-1.0)*x1955*x1958))+(((0.087975)*x1956))+(((-0.027)*x1957))+(((0.167025)*sj9))+(((-1.0)*(0.01125)*cj9))+(((-1.0)*x1954*x1958)))),((-0.304525)+(((-1.0)*x1959*x1960))+((x1960*(px*px)))+(((-1.0)*(0.0495)*sj9))+(((-0.087975)*x1957))+(((-1.0)*(0.33)*cj9))+(((2.0)*cj4*px*x1955))+(((-0.027)*x1956))+x1959),IKFAST_ATAN2_MAGTHRESH);
if(!x1961.valid){
continue;
}
CheckValue<IkReal> x1962=IKPowWithIntegerCheck(IKsign(((((-1.0)*(0.55)*pz))+((x1953*x1954))+(((-1.0)*cj9*x1952))+((x1953*x1955))+(((-1.0)*cj9*x1951))+x1951+x1952+(((-1.0)*(0.045)*pz*sj9))+(((-1.0)*(0.3)*cj9*pz)))),-1);
if(!x1962.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x1961.value)+(((1.5707963267949)*(x1962.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1963=((0.3)*cj9);
IkReal x1964=((0.045)*sj9);
IkReal x1965=IKcos(j6);
IkReal x1966=(pz*x1965);
IkReal x1967=IKsin(j6);
IkReal x1968=(cj4*px);
IkReal x1969=(x1967*x1968);
IkReal x1970=(py*sj4);
IkReal x1971=(x1967*x1970);
IkReal x1972=((0.045)*cj9);
IkReal x1973=((0.3)*sj9);
IkReal x1974=(pz*x1967);
IkReal x1975=((1.0)*x1968);
IkReal x1976=((1.0)*x1970);
IkReal x1977=((0.09)*x1965);
evalcond[0]=((-0.55)+x1966+x1969+(((-1.0)*x1964))+x1971+(((-1.0)*x1963)));
evalcond[1]=((0.045)+(((-1.0)*x1965*x1976))+(((-1.0)*x1965*x1975))+(((-1.0)*x1972))+x1973+x1974);
evalcond[2]=((((-0.316735294117647)*x1965))+(((-0.588235294117647)*pp*x1965))+pz+(((-1.51009803921569)*x1967))+(((-0.108264705882353)*cj9*x1965))+(((3.92156862745098)*pp*x1967))+(((-1.32323529411765)*cj9*x1967)));
evalcond[3]=((((0.045)*x1965))+((x1964*x1967))+((x1963*x1967))+(((-1.0)*x1976))+(((-1.0)*x1965*x1972))+(((-1.0)*x1975))+(((0.55)*x1967))+((x1965*x1973)));
evalcond[4]=((-0.2125)+(((1.1)*x1966))+(((1.1)*x1969))+(((1.1)*x1971))+(((-0.09)*x1974))+((x1970*x1977))+((x1968*x1977))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x1978=(cj4*px);
IkReal x1979=(py*sj4);
IkReal x1980=((1.32323529411765)*cj9);
IkReal x1981=((3.92156862745098)*pp);
IkReal x1982=((0.0264705882352941)*pp);
IkReal x1983=(cj9*sj9);
IkReal x1984=((0.176470588235294)*pp);
IkReal x1985=cj9*cj9;
CheckValue<IkReal> x1986 = IKatan2WithCheck(IkReal(((-0.0142530882352941)+((pz*x1979))+(((0.00938117647058823)*cj9))+((cj9*x1982))+(((-0.0324794117647059)*x1983))+(((0.00487191176470588)*x1985))+(((-1.0)*x1982))+((pz*x1978))+(((-1.0)*sj9*x1984))+(((-1.0)*(0.0950205882352941)*sj9)))),((0.0679544117647059)+(((0.396970588235294)*x1983))+(((-0.0595455882352941)*x1985))+(pz*pz)+((cj9*x1984))+(((-1.0)*(0.00840882352941177)*cj9))+(((0.453029411764706)*sj9))+(((-1.0)*x1984))+(((-1.0)*(1.17647058823529)*pp*sj9))),IKFAST_ATAN2_MAGTHRESH);
if(!x1986.valid){
continue;
}
CheckValue<IkReal> x1987=IKPowWithIntegerCheck(IKsign(((((-1.0)*x1978*x1981))+(((1.51009803921569)*x1978))+(((1.51009803921569)*x1979))+(((0.316735294117647)*pz))+(((0.108264705882353)*cj9*pz))+(((0.588235294117647)*pp*pz))+((x1978*x1980))+(((-1.0)*x1979*x1981))+((x1979*x1980)))),-1);
if(!x1987.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x1986.value)+(((1.5707963267949)*(x1987.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x1988=((0.3)*cj9);
IkReal x1989=((0.045)*sj9);
IkReal x1990=IKcos(j6);
IkReal x1991=(pz*x1990);
IkReal x1992=IKsin(j6);
IkReal x1993=(cj4*px);
IkReal x1994=(x1992*x1993);
IkReal x1995=(py*sj4);
IkReal x1996=(x1992*x1995);
IkReal x1997=((0.045)*cj9);
IkReal x1998=((0.3)*sj9);
IkReal x1999=(pz*x1992);
IkReal x2000=((1.0)*x1993);
IkReal x2001=((1.0)*x1995);
IkReal x2002=((0.09)*x1990);
evalcond[0]=((-0.55)+(((-1.0)*x1989))+x1996+x1994+x1991+(((-1.0)*x1988)));
evalcond[1]=((0.045)+(((-1.0)*x1990*x2000))+(((-1.0)*x1997))+x1998+x1999+(((-1.0)*x1990*x2001)));
evalcond[2]=((((-0.108264705882353)*cj9*x1990))+(((-0.588235294117647)*pp*x1990))+pz+(((-1.51009803921569)*x1992))+(((-1.32323529411765)*cj9*x1992))+(((3.92156862745098)*pp*x1992))+(((-0.316735294117647)*x1990)));
evalcond[3]=((((0.045)*x1990))+(((-1.0)*x2001))+(((0.55)*x1992))+(((-1.0)*x2000))+(((-1.0)*x1990*x1997))+((x1990*x1998))+((x1989*x1992))+((x1988*x1992)));
evalcond[4]=((-0.2125)+(((1.1)*x1996))+(((1.1)*x1991))+((x1995*x2002))+(((-0.09)*x1999))+((x1993*x2002))+(((-1.0)*(1.0)*pp))+(((1.1)*x1994)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x2003=(px*sj4);
IkReal x2004=(cj4*py);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j8)))), 6.28318530717959)));
evalcond[1]=((0.39655)+(((0.0765)*sj9))+(((0.32595)*cj9))+(((-1.0)*(1.0)*pp)));
evalcond[2]=(x2003+(((-1.0)*x2004)));
evalcond[3]=((((-1.0)*x2003))+x2004);
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x2005=py*py;
IkReal x2006=cj4*cj4;
IkReal x2007=(x2005+(((-1.0)*x2005*x2006))+(pz*pz)+(((2.0)*cj4*px*py*sj4))+((x2006*(px*px))));
j6eval[0]=x2007;
j6eval[1]=IKsign(x2007);
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x2008=(cj4*px);
IkReal x2009=(cj9*pz);
IkReal x2010=(py*sj4);
IkReal x2011=(pz*sj9);
IkReal x2012=(cj4*px*sj9);
IkReal x2013=(py*sj4*sj9);
IkReal x2014=((0.045)*x2008);
IkReal x2015=((0.045)*x2010);
j6eval[0]=(((cj9*x2010))+(((-1.0)*x2010))+((cj9*x2008))+(((-1.0)*(12.2222222222222)*pz))+(((-1.0)*x2011))+(((-6.66666666666667)*x2009))+(((-6.66666666666667)*x2013))+(((-1.0)*x2008))+(((-6.66666666666667)*x2012)));
j6eval[1]=IKsign((((cj9*x2014))+(((-0.3)*x2012))+(((-0.3)*x2013))+(((-1.0)*(0.55)*pz))+(((-0.045)*x2011))+((cj9*x2015))+(((-1.0)*x2014))+(((-1.0)*x2015))+(((-0.3)*x2009))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
IkReal x2016=(cj4*px);
IkReal x2017=(cj9*pz);
IkReal x2018=(pp*pz);
IkReal x2019=(py*sj4);
IkReal x2020=(cj4*cj9*px);
IkReal x2021=(cj4*pp*px);
IkReal x2022=(cj9*py*sj4);
IkReal x2023=(pp*py*sj4);
j6eval[0]=((((-2.92556370551481)*x2016))+(((-1.0)*(13.9482024812098)*pz))+(((-5.4333061668025)*x2021))+(((-1.0)*x2022))+(((-12.2222222222222)*x2017))+(((-5.4333061668025)*x2023))+(((-2.92556370551481)*x2019))+(((36.2220411120167)*x2018))+(((-1.0)*x2020)));
j6eval[1]=IKsign(((((-0.588235294117647)*x2021))+(((-1.32323529411765)*x2017))+(((3.92156862745098)*x2018))+(((-0.316735294117647)*x2019))+(((-1.0)*(1.51009803921569)*pz))+(((-0.108264705882353)*x2020))+(((-0.108264705882353)*x2022))+(((-0.588235294117647)*x2023))+(((-0.316735294117647)*x2016))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[1];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j9), 6.28318530717959)))))+(IKabs(pz)));
if( IKabs(evalcond[0]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x2024=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x2024);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x2024);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x2024);
rxp2_1=(px*r22);
j6eval[0]=((((-1.0)*(1.0)*cj4*px))+(((-1.0)*(1.0)*py*sj4)));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[1];
IkReal x2025=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x2025);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x2025);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x2025);
rxp2_1=(px*r22);
j6eval[0]=((-1.0)+(((-1.0)*(1.3840830449827)*(px*px)))+(((-1.0)*(1.3840830449827)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
{
IkReal j6eval[2];
IkReal x2026=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=((py*py)+(px*px));
npx=(((py*r10))+((px*r00)));
npy=(((py*r11))+((px*r01)));
npz=(((px*r02))+((py*r12)));
rxp0_0=((-1.0)*r20*x2026);
rxp0_1=(px*r20);
rxp1_0=((-1.0)*r21*x2026);
rxp1_1=(px*r21);
rxp2_0=((-1.0)*r22*x2026);
rxp2_1=(px*r22);
IkReal x2027=(cj4*px);
IkReal x2028=(py*sj4);
j6eval[0]=(x2027+x2028);
j6eval[1]=((((-1.0)*(1.3840830449827)*cj4*(px*px*px)))+(((-1.0)*x2028))+(((-1.3840830449827)*x2027*(py*py)))+(((-1.0)*(1.3840830449827)*sj4*(py*py*py)))+(((-1.0)*x2027))+(((-1.3840830449827)*x2028*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  || IKabs(j6eval[1]) < 0.0000010000000000  )
{
{
IkReal evalcond[4];
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=-0.2125;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
j6array[0]=0.148889947609497;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=3.29048260119929;
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((3.14159265358979)+j4), 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x2029=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x2029);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x2029);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x2029);
rxp2_1=0;
px=0;
j4=0;
sj4=0;
cj4=1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x2030=py*py;
CheckValue<IkReal> x2032 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x2030)))),((2.83333333333333)+(((-3.92156862745098)*x2030))),IKFAST_ATAN2_MAGTHRESH);
if(!x2032.valid){
continue;
}
IkReal x2031=((-1.0)*(x2032.value));
j6array[0]=x2031;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x2031);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(j4, 6.28318530717959)))))+(IKabs(px)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(py*py))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x2033=((1.0)*py);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=py*py;
npx=(py*r10);
npy=(py*r11);
npz=(py*r12);
rxp0_0=((-1.0)*r20*x2033);
rxp0_1=0;
rxp1_0=((-1.0)*r21*x2033);
rxp1_1=0;
rxp2_0=((-1.0)*r22*x2033);
rxp2_1=0;
px=0;
j4=3.14159265358979;
sj4=0;
cj4=-1.0;
rxp0_2=(py*r00);
rxp1_2=(py*r01);
rxp2_2=(py*r02);
j6eval[0]=((1.0)+(((1.91568587540858)*(py*py*py*py)))+(((-1.0)*(2.64633970947792)*(py*py))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x2034=py*py;
CheckValue<IkReal> x2036 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x2034)))),((2.83333333333333)+(((-3.92156862745098)*x2034))),IKFAST_ATAN2_MAGTHRESH);
if(!x2036.valid){
continue;
}
IkReal x2035=((-1.0)*(x2036.value));
j6array[0]=x2035;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x2035);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(((-3.14159265358979)+(IKfmod(((1.5707963267949)+j4), 6.28318530717959)))))+(IKabs(py)));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x2037=((1.0)*px);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=1.5707963267949;
sj4=1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x2037);
rxp1_2=((-1.0)*r11*x2037);
rxp2_2=((-1.0)*r12*x2037);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x2038=px*px;
CheckValue<IkReal> x2040 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x2038)))),((2.83333333333333)+(((-3.92156862745098)*x2038))),IKFAST_ATAN2_MAGTHRESH);
if(!x2040.valid){
continue;
}
IkReal x2039=((-1.0)*(x2040.value));
j6array[0]=x2039;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x2039);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((IKabs(py))+(IKabs(((-3.14159265358979)+(IKfmod(((4.71238898038469)+j4), 6.28318530717959))))));
evalcond[1]=-0.85;
evalcond[2]=0;
evalcond[3]=((-0.2125)+(((-1.0)*(1.0)*(px*px))));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j6eval[1];
IkReal x2041=((1.0)*px);
sj8=0;
cj8=-1.0;
j8=3.14159265358979;
pz=0;
j9=0;
sj9=0;
cj9=1.0;
pp=px*px;
npx=(px*r00);
npy=(px*r01);
npz=(px*r02);
rxp0_0=0;
rxp0_1=(px*r20);
rxp1_0=0;
rxp1_1=(px*r21);
rxp2_0=0;
rxp2_1=(px*r22);
py=0;
j4=-1.5707963267949;
sj4=-1.0;
cj4=0;
rxp0_2=((-1.0)*r10*x2041);
rxp1_2=((-1.0)*r11*x2041);
rxp2_2=((-1.0)*r12*x2041);
j6eval[0]=((1.0)+(((1.91568587540858)*(px*px*px*px)))+(((-1.0)*(2.64633970947792)*(px*px))));
if( IKabs(j6eval[0]) < 0.0000010000000000  )
{
continue; // no branches [j6]

} else
{
{
IkReal j6array[2], cj6array[2], sj6array[2];
bool j6valid[2]={false};
_nj6 = 2;
IkReal x2042=px*px;
CheckValue<IkReal> x2044 = IKatan2WithCheck(IkReal(((-0.425)+(((-0.588235294117647)*x2042)))),((2.83333333333333)+(((-3.92156862745098)*x2042))),IKFAST_ATAN2_MAGTHRESH);
if(!x2044.valid){
continue;
}
IkReal x2043=((-1.0)*(x2044.value));
j6array[0]=x2043;
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
j6array[1]=((3.14159265358979)+x2043);
sj6array[1]=IKsin(j6array[1]);
cj6array[1]=IKcos(j6array[1]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
if( j6array[1] > IKPI )
{
    j6array[1]-=IK2PI;
}
else if( j6array[1] < -IKPI )
{    j6array[1]+=IK2PI;
}
j6valid[1] = true;
for(int ij6 = 0; ij6 < 2; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 2; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[1];
evalcond[0]=((0.85)*(IKsin(j6)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2045=(cj4*px);
IkReal x2046=(py*sj4);
IkReal x2047=px*px;
IkReal x2048=py*py;
CheckValue<IkReal> x2049=IKPowWithIntegerCheck(((((20.0)*x2046))+(((20.0)*x2045))),-1);
if(!x2049.valid){
continue;
}
CheckValue<IkReal> x2050=IKPowWithIntegerCheck(((((-8.5)*x2046))+(((-1.0)*(11.7647058823529)*cj4*(px*px*px)))+(((-8.5)*x2045))+(((-11.7647058823529)*x2045*x2048))+(((-11.7647058823529)*x2046*x2047))+(((-1.0)*(11.7647058823529)*sj4*(py*py*py)))),-1);
if(!x2050.valid){
continue;
}
if( IKabs(((17.0)*(x2049.value))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x2050.value)*(((-48.1666666666667)+(((66.6666666666667)*x2047))+(((66.6666666666667)*x2048)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((17.0)*(x2049.value)))+IKsqr(((x2050.value)*(((-48.1666666666667)+(((66.6666666666667)*x2047))+(((66.6666666666667)*x2048))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((17.0)*(x2049.value)), ((x2050.value)*(((-48.1666666666667)+(((66.6666666666667)*x2047))+(((66.6666666666667)*x2048))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2051=IKcos(j6);
IkReal x2052=(cj4*px);
IkReal x2053=(x2051*x2052);
IkReal x2054=(py*sj4);
IkReal x2055=(x2051*x2054);
IkReal x2056=IKsin(j6);
IkReal x2057=(x2052*x2056);
IkReal x2058=(x2054*x2056);
IkReal x2059=px*px;
IkReal x2060=((3.92156862745098)*x2056);
IkReal x2061=((0.588235294117647)*x2051);
IkReal x2062=py*py;
evalcond[0]=(x2055+x2053);
evalcond[1]=((-0.85)+x2057+x2058);
evalcond[2]=((((0.85)*x2056))+(((-1.0)*x2052))+(((-1.0)*x2054)));
evalcond[3]=((((-1.0)*x2061*x2062))+(((-1.0)*x2059*x2061))+(((-0.425)*x2051))+(((-1.0)*x2059*x2060))+(((2.83333333333333)*x2056))+(((-1.0)*x2060*x2062)));
evalcond[4]=((-0.2125)+(((1.1)*x2057))+(((-1.0)*x2059))+(((-0.09)*x2055))+(((1.1)*x2058))+(((-1.0)*x2062))+(((-0.09)*x2053)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2063=(cj4*px);
IkReal x2064=(py*sj4);
IkReal x2065=px*px;
IkReal x2066=py*py;
CheckValue<IkReal> x2067=IKPowWithIntegerCheck(((-7.225)+(((-10.0)*x2066))+(((-10.0)*x2065))),-1);
if(!x2067.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x2063))+(((1.17647058823529)*x2064)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x2067.value)*(((((-56.6666666666667)*x2064))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((78.4313725490196)*x2063*x2066))+(((78.4313725490196)*x2064*x2065))+(((-56.6666666666667)*x2063)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x2063))+(((1.17647058823529)*x2064))))+IKsqr(((x2067.value)*(((((-56.6666666666667)*x2064))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((78.4313725490196)*x2063*x2066))+(((78.4313725490196)*x2064*x2065))+(((-56.6666666666667)*x2063))))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x2063))+(((1.17647058823529)*x2064))), ((x2067.value)*(((((-56.6666666666667)*x2064))+(((78.4313725490196)*sj4*(py*py*py)))+(((78.4313725490196)*cj4*(px*px*px)))+(((78.4313725490196)*x2063*x2066))+(((78.4313725490196)*x2064*x2065))+(((-56.6666666666667)*x2063))))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2068=IKcos(j6);
IkReal x2069=(cj4*px);
IkReal x2070=(x2068*x2069);
IkReal x2071=(py*sj4);
IkReal x2072=(x2068*x2071);
IkReal x2073=IKsin(j6);
IkReal x2074=(x2069*x2073);
IkReal x2075=(x2071*x2073);
IkReal x2076=px*px;
IkReal x2077=((3.92156862745098)*x2073);
IkReal x2078=((0.588235294117647)*x2068);
IkReal x2079=py*py;
evalcond[0]=(x2070+x2072);
evalcond[1]=((-0.85)+x2075+x2074);
evalcond[2]=((((-1.0)*x2071))+(((0.85)*x2073))+(((-1.0)*x2069)));
evalcond[3]=((((-1.0)*x2076*x2077))+(((-1.0)*x2076*x2078))+(((-0.425)*x2068))+(((-1.0)*x2078*x2079))+(((2.83333333333333)*x2073))+(((-1.0)*x2077*x2079)));
evalcond[4]=((-0.2125)+(((-1.0)*x2079))+(((-1.0)*x2076))+(((1.1)*x2075))+(((-0.09)*x2070))+(((1.1)*x2074))+(((-0.09)*x2072)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2080=(cj4*px);
IkReal x2081=(py*sj4);
IkReal x2082=px*px;
IkReal x2083=py*py;
IkReal x2084=((1.29411764705882)*(cj4*cj4));
CheckValue<IkReal> x2085=IKPowWithIntegerCheck(((((-0.09)*x2080))+(((-0.09)*x2081))),-1);
if(!x2085.valid){
continue;
}
if( IKabs(((((1.17647058823529)*x2080))+(((1.17647058823529)*x2081)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((x2085.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x2081))+(((-0.294117647058824)*x2083))+((x2083*x2084))+(((-1.0)*x2082*x2084))+x2082)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((1.17647058823529)*x2080))+(((1.17647058823529)*x2081))))+IKsqr(((x2085.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x2081))+(((-0.294117647058824)*x2083))+((x2083*x2084))+(((-1.0)*x2082*x2084))+x2082))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j6array[0]=IKatan2(((((1.17647058823529)*x2080))+(((1.17647058823529)*x2081))), ((x2085.value)*(((0.2125)+(((-2.58823529411765)*cj4*px*x2081))+(((-0.294117647058824)*x2083))+((x2083*x2084))+(((-1.0)*x2082*x2084))+x2082))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2086=IKcos(j6);
IkReal x2087=(cj4*px);
IkReal x2088=(x2086*x2087);
IkReal x2089=(py*sj4);
IkReal x2090=(x2086*x2089);
IkReal x2091=IKsin(j6);
IkReal x2092=(x2087*x2091);
IkReal x2093=(x2089*x2091);
IkReal x2094=px*px;
IkReal x2095=((3.92156862745098)*x2091);
IkReal x2096=((0.588235294117647)*x2086);
IkReal x2097=py*py;
evalcond[0]=(x2090+x2088);
evalcond[1]=((-0.85)+x2093+x2092);
evalcond[2]=((((-1.0)*x2089))+(((-1.0)*x2087))+(((0.85)*x2091)));
evalcond[3]=((((-1.0)*x2095*x2097))+(((-1.0)*x2094*x2096))+(((-1.0)*x2096*x2097))+(((2.83333333333333)*x2091))+(((-1.0)*x2094*x2095))+(((-0.425)*x2086)));
evalcond[4]=((-0.2125)+(((-1.0)*x2094))+(((1.1)*x2093))+(((1.1)*x2092))+(((-1.0)*x2097))+(((-0.09)*x2090))+(((-0.09)*x2088)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2098=(cj4*px);
IkReal x2099=(py*sj4);
IkReal x2100=((0.108264705882353)*cj9);
IkReal x2101=((0.588235294117647)*pp);
IkReal x2102=(cj9*pp);
IkReal x2103=(cj9*sj9);
IkReal x2104=(pp*sj9);
IkReal x2105=cj9*cj9;
IkReal x2106=((1.0)*pz);
CheckValue<IkReal> x2107=IKPowWithIntegerCheck(IKsign(((((-1.0)*x2099*x2100))+(((-1.0)*x2098*x2101))+(((-1.0)*(1.32323529411765)*cj9*pz))+(((3.92156862745098)*pp*pz))+(((-1.0)*x2098*x2100))+(((-1.0)*(1.51009803921569)*pz))+(((-0.316735294117647)*x2098))+(((-1.0)*x2099*x2101))+(((-0.316735294117647)*x2099)))),-1);
if(!x2107.valid){
continue;
}
CheckValue<IkReal> x2108 = IKatan2WithCheck(IkReal(((-0.174204411764706)+(((-0.0324794117647059)*x2105))+(pz*pz)+(((-0.0264705882352941)*x2104))+(((-1.0)*(0.154566176470588)*cj9))+(((-1.0)*(0.323529411764706)*pp))+(((-0.00487191176470588)*x2103))+(((-1.0)*(0.0142530882352941)*sj9))+(((-0.176470588235294)*x2102)))),((-0.830553921568627)+(((-1.0)*(0.0679544117647059)*sj9))+(((-1.0)*(1.18080882352941)*cj9))+(((-0.396970588235294)*x2105))+(((-1.0)*x2099*x2106))+(((-0.0595455882352941)*x2103))+(((2.15686274509804)*pp))+(((-1.0)*x2098*x2106))+(((0.176470588235294)*x2104))+(((1.17647058823529)*x2102))),IKFAST_ATAN2_MAGTHRESH);
if(!x2108.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x2107.value)))+(x2108.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2109=((0.3)*cj9);
IkReal x2110=((0.045)*sj9);
IkReal x2111=IKcos(j6);
IkReal x2112=(pz*x2111);
IkReal x2113=IKsin(j6);
IkReal x2114=(cj4*px);
IkReal x2115=(x2113*x2114);
IkReal x2116=(py*sj4);
IkReal x2117=(x2113*x2116);
IkReal x2118=((0.045)*cj9);
IkReal x2119=((0.3)*sj9);
IkReal x2120=(pz*x2113);
IkReal x2121=(x2111*x2114);
IkReal x2122=(x2111*x2116);
evalcond[0]=((-0.55)+x2115+x2117+x2112+(((-1.0)*x2110))+(((-1.0)*x2109)));
evalcond[1]=((0.045)+x2119+x2121+x2122+(((-1.0)*x2118))+(((-1.0)*x2120)));
evalcond[2]=((((-3.92156862745098)*pp*x2113))+pz+(((-0.316735294117647)*x2111))+(((-0.588235294117647)*pp*x2111))+(((1.32323529411765)*cj9*x2113))+(((-0.108264705882353)*cj9*x2111))+(((1.51009803921569)*x2113)));
evalcond[3]=((((-1.0)*x2116))+(((-1.0)*x2114))+((x2111*x2118))+(((-1.0)*x2111*x2119))+((x2110*x2113))+(((0.55)*x2113))+(((-0.045)*x2111))+((x2109*x2113)));
evalcond[4]=((-0.2125)+(((1.1)*x2115))+(((1.1)*x2112))+(((1.1)*x2117))+(((-0.09)*x2122))+(((-1.0)*(1.0)*pp))+(((0.09)*x2120))+(((-0.09)*x2121)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2123=((0.045)*cj4*px);
IkReal x2124=((0.045)*py*sj4);
IkReal x2125=((0.3)*sj9);
IkReal x2126=(cj4*px);
IkReal x2127=(py*sj4);
IkReal x2128=(cj9*sj9);
IkReal x2129=cj9*cj9;
IkReal x2130=((1.0)*pz);
IkReal x2131=py*py;
IkReal x2132=cj4*cj4;
CheckValue<IkReal> x2133=IKPowWithIntegerCheck(IKsign(((((-1.0)*(0.55)*pz))+(((-1.0)*x2125*x2127))+(((-1.0)*x2125*x2126))+((cj9*x2123))+(((-1.0)*x2123))+((cj9*x2124))+(((-1.0)*x2124))+(((-1.0)*(0.045)*pz*sj9))+(((-1.0)*(0.3)*cj9*pz)))),-1);
if(!x2133.valid){
continue;
}
CheckValue<IkReal> x2134 = IKatan2WithCheck(IkReal(((-0.03825)+(((-0.087975)*x2128))+(((0.027)*x2129))+(((-1.0)*(0.167025)*sj9))+(((-1.0)*x2126*x2130))+(((-1.0)*x2127*x2130))+(((0.01125)*cj9)))),((-0.304525)+((x2132*(px*px)))+(((-1.0)*(0.0495)*sj9))+(((-0.027)*x2128))+(((-1.0)*x2131*x2132))+(((-1.0)*(0.33)*cj9))+(((-0.087975)*x2129))+(((2.0)*cj4*px*x2127))+x2131),IKFAST_ATAN2_MAGTHRESH);
if(!x2134.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x2133.value)))+(x2134.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2135=((0.3)*cj9);
IkReal x2136=((0.045)*sj9);
IkReal x2137=IKcos(j6);
IkReal x2138=(pz*x2137);
IkReal x2139=IKsin(j6);
IkReal x2140=(cj4*px);
IkReal x2141=(x2139*x2140);
IkReal x2142=(py*sj4);
IkReal x2143=(x2139*x2142);
IkReal x2144=((0.045)*cj9);
IkReal x2145=((0.3)*sj9);
IkReal x2146=(pz*x2139);
IkReal x2147=(x2137*x2140);
IkReal x2148=(x2137*x2142);
evalcond[0]=((-0.55)+(((-1.0)*x2136))+x2141+x2143+(((-1.0)*x2135))+x2138);
evalcond[1]=((0.045)+x2148+x2145+x2147+(((-1.0)*x2144))+(((-1.0)*x2146)));
evalcond[2]=((((-0.588235294117647)*pp*x2137))+(((1.32323529411765)*cj9*x2139))+(((1.51009803921569)*x2139))+pz+(((-3.92156862745098)*pp*x2139))+(((-0.316735294117647)*x2137))+(((-0.108264705882353)*cj9*x2137)));
evalcond[3]=((((0.55)*x2139))+(((-0.045)*x2137))+(((-1.0)*x2140))+((x2135*x2139))+(((-1.0)*x2142))+((x2136*x2139))+((x2137*x2144))+(((-1.0)*x2137*x2145)));
evalcond[4]=((-0.2125)+(((-0.09)*x2148))+(((1.1)*x2143))+(((-0.09)*x2147))+(((1.1)*x2141))+(((0.09)*x2146))+(((1.1)*x2138))+(((-1.0)*(1.0)*pp)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2149=py*py;
IkReal x2150=(py*sj4);
IkReal x2151=cj4*cj4;
IkReal x2152=((0.045)*pz);
IkReal x2153=(cj4*px);
IkReal x2154=((0.3)*pz);
IkReal x2155=((0.3)*cj4*px);
IkReal x2156=((0.045)*x2153);
IkReal x2157=((0.3)*py*sj4);
IkReal x2158=((0.045)*x2150);
CheckValue<IkReal> x2159=IKPowWithIntegerCheck(IKsign(((((-1.0)*x2149*x2151))+(pz*pz)+x2149+(((2.0)*cj4*px*x2150))+((x2151*(px*px))))),-1);
if(!x2159.valid){
continue;
}
CheckValue<IkReal> x2160 = IKatan2WithCheck(IkReal((((sj9*x2158))+x2152+(((0.55)*x2150))+((cj9*x2155))+(((0.55)*x2153))+((sj9*x2156))+((sj9*x2154))+(((-1.0)*cj9*x2152))+((cj9*x2157)))),((((-1.0)*sj9*x2157))+(((-1.0)*x2156))+((cj9*x2154))+((sj9*x2152))+(((-1.0)*sj9*x2155))+((cj9*x2156))+(((-1.0)*x2158))+(((0.55)*pz))+((cj9*x2158))),IKFAST_ATAN2_MAGTHRESH);
if(!x2160.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x2159.value)))+(x2160.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[5];
IkReal x2161=((0.3)*cj9);
IkReal x2162=((0.045)*sj9);
IkReal x2163=IKcos(j6);
IkReal x2164=(pz*x2163);
IkReal x2165=IKsin(j6);
IkReal x2166=(cj4*px);
IkReal x2167=(x2165*x2166);
IkReal x2168=(py*sj4);
IkReal x2169=(x2165*x2168);
IkReal x2170=((0.045)*cj9);
IkReal x2171=((0.3)*sj9);
IkReal x2172=(pz*x2165);
IkReal x2173=(x2163*x2166);
IkReal x2174=(x2163*x2168);
evalcond[0]=((-0.55)+(((-1.0)*x2162))+x2169+x2164+x2167+(((-1.0)*x2161)));
evalcond[1]=((0.045)+x2173+x2174+x2171+(((-1.0)*x2172))+(((-1.0)*x2170)));
evalcond[2]=((((1.32323529411765)*cj9*x2165))+(((-0.108264705882353)*cj9*x2163))+(((-3.92156862745098)*pp*x2165))+(((-0.588235294117647)*pp*x2163))+pz+(((1.51009803921569)*x2165))+(((-0.316735294117647)*x2163)));
evalcond[3]=(((x2163*x2170))+(((-1.0)*x2168))+((x2161*x2165))+(((-1.0)*x2163*x2171))+(((-0.045)*x2163))+((x2162*x2165))+(((-1.0)*x2166))+(((0.55)*x2165)));
evalcond[4]=((-0.2125)+(((-0.09)*x2174))+(((0.09)*x2172))+(((1.1)*x2164))+(((1.1)*x2167))+(((1.1)*x2169))+(((-1.0)*(1.0)*pp))+(((-0.09)*x2173)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j6]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2175=(pz*sj8);
IkReal x2176=((0.3)*cj9);
IkReal x2177=((0.045)*sj9);
IkReal x2178=(cj4*px);
IkReal x2179=((0.045)*cj8*sj8);
IkReal x2180=(x2178*x2179);
IkReal x2181=(py*sj4);
IkReal x2182=(x2179*x2181);
IkReal x2183=((0.3)*cj8*sj8*sj9);
IkReal x2184=((0.55)*cj8);
IkReal x2185=(cj4*py);
IkReal x2186=(px*sj4);
IkReal x2187=(cj4*cj8*py);
IkReal x2188=((1.0)*pz*sj8);
IkReal x2189=(cj8*px*sj4);
IkReal x2190=cj8*cj8;
IkReal x2191=((0.045)*x2190);
IkReal x2192=(x2185*x2191);
IkReal x2193=(x2186*x2191);
IkReal x2194=((0.3)*sj9*x2190);
CheckValue<IkReal> x2195=IKPowWithIntegerCheck(IKsign(((((-1.0)*cj9*x2182))+((x2181*x2183))+(((-0.55)*x2175))+(((-1.0)*x2175*x2177))+(((-1.0)*x2175*x2176))+(((-1.0)*cj9*x2180))+((x2178*x2183))+x2182+x2180)),-1);
if(!x2195.valid){
continue;
}
CheckValue<IkReal> x2196 = IKatan2WithCheck(IkReal(((((-1.0)*x2176*x2189))+((x2184*x2185))+((x2176*x2187))+(((-1.0)*x2178*x2188))+((x2177*x2187))+(((-1.0)*x2181*x2188))+(((-1.0)*x2184*x2186))+(((-1.0)*x2177*x2189)))),((((-1.0)*(1.0)*sj8*(pz*pz)))+(((-1.0)*x2193))+x2192+(((-1.0)*x2186*x2194))+(((-1.0)*cj9*x2192))+((x2185*x2194))+((cj9*x2193))),IKFAST_ATAN2_MAGTHRESH);
if(!x2196.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(((1.5707963267949)*(x2195.value)))+(x2196.value));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x2197=((0.3)*cj9);
IkReal x2198=((0.045)*sj9);
IkReal x2199=IKcos(j6);
IkReal x2200=(pz*x2199);
IkReal x2201=IKsin(j6);
IkReal x2202=(cj4*px*x2201);
IkReal x2203=(py*sj4);
IkReal x2204=(x2201*x2203);
IkReal x2205=(px*sj4);
IkReal x2206=((1.0)*cj4*py);
IkReal x2207=(cj4*sj8);
IkReal x2208=((0.045)*cj8);
IkReal x2209=((0.045)*cj9);
IkReal x2210=(cj8*x2201);
IkReal x2211=((0.3)*sj9);
IkReal x2212=(sj8*x2205);
IkReal x2213=(pz*x2210);
IkReal x2214=(px*(((1.0)*cj4)));
IkReal x2215=(cj8*x2199);
IkReal x2216=((1.0)*x2203);
IkReal x2217=((0.09)*cj8*x2199);
evalcond[0]=((-0.55)+x2204+x2202+x2200+(((-1.0)*x2198))+(((-1.0)*x2197)));
evalcond[1]=(((cj8*x2205))+(((-1.0)*pz*sj8*x2201))+(((-1.0)*cj8*x2206))+((sj8*x2199*x2203))+((px*x2199*x2207)));
evalcond[2]=((((-0.55)*x2199))+(((-1.0)*x2197*x2199))+((x2201*x2208))+pz+(((-1.0)*x2209*x2210))+(((-1.0)*x2198*x2199))+((x2210*x2211)));
evalcond[3]=((0.045)+(((-1.0)*x2215*x2216))+(((-1.0)*x2209))+x2211+x2213+x2212+(((-1.0)*x2214*x2215))+(((-1.0)*sj8*x2206)));
evalcond[4]=(((x2211*x2215))+((x2198*x2201))+(((-1.0)*x2214))+(((-1.0)*x2209*x2215))+(((0.55)*x2201))+((x2199*x2208))+(((-1.0)*x2216))+((x2197*x2201)));
evalcond[5]=((-0.2125)+(((1.1)*x2204))+(((1.1)*x2202))+(((0.09)*py*x2207))+(((-0.09)*x2213))+(((-0.09)*x2212))+((x2203*x2217))+(((-1.0)*(1.0)*pp))+((cj4*px*x2217))+(((1.1)*x2200)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2218=py*py;
IkReal x2219=(sj8*x2218);
IkReal x2220=(cj4*px*sj8);
IkReal x2221=cj4*cj4;
IkReal x2222=px*px;
IkReal x2223=((0.55)*sj8);
IkReal x2224=(cj8*px);
IkReal x2225=((0.3)*cj9);
IkReal x2226=((0.045)*sj9);
IkReal x2227=(py*sj4*sj8);
IkReal x2228=(pz*sj8);
IkReal x2229=(cj4*cj8*sj4);
CheckValue<IkReal> x2230 = IKatan2WithCheck(IkReal((((py*sj4*x2223))+((x2220*x2225))+((pz*sj4*x2224))+(((-1.0)*cj4*cj8*py*pz))+((cj4*px*x2223))+((x2226*x2227))+((x2225*x2227))+((x2220*x2226)))),((((2.0)*py*x2221*x2224))+((x2225*x2228))+((x2218*x2229))+((x2226*x2228))+((pz*x2223))+(((-1.0)*x2222*x2229))+(((-1.0)*py*x2224))),IKFAST_ATAN2_MAGTHRESH);
if(!x2230.valid){
continue;
}
CheckValue<IkReal> x2231=IKPowWithIntegerCheck(IKsign(((((-1.0)*x2219*x2221))+(((2.0)*py*sj4*x2220))+x2219+((sj8*(pz*pz)))+((sj8*x2221*x2222)))),-1);
if(!x2231.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x2230.value)+(((1.5707963267949)*(x2231.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x2232=((0.3)*cj9);
IkReal x2233=((0.045)*sj9);
IkReal x2234=IKcos(j6);
IkReal x2235=(pz*x2234);
IkReal x2236=IKsin(j6);
IkReal x2237=(cj4*px*x2236);
IkReal x2238=(py*sj4);
IkReal x2239=(x2236*x2238);
IkReal x2240=(px*sj4);
IkReal x2241=((1.0)*cj4*py);
IkReal x2242=(cj4*sj8);
IkReal x2243=((0.045)*cj8);
IkReal x2244=((0.045)*cj9);
IkReal x2245=(cj8*x2236);
IkReal x2246=((0.3)*sj9);
IkReal x2247=(sj8*x2240);
IkReal x2248=(pz*x2245);
IkReal x2249=(px*(((1.0)*cj4)));
IkReal x2250=(cj8*x2234);
IkReal x2251=((1.0)*x2238);
IkReal x2252=((0.09)*cj8*x2234);
evalcond[0]=((-0.55)+x2237+x2235+x2239+(((-1.0)*x2232))+(((-1.0)*x2233)));
evalcond[1]=(((px*x2234*x2242))+((sj8*x2234*x2238))+(((-1.0)*cj8*x2241))+((cj8*x2240))+(((-1.0)*pz*sj8*x2236)));
evalcond[2]=((((-1.0)*x2244*x2245))+((x2236*x2243))+pz+(((-1.0)*x2233*x2234))+(((-1.0)*x2232*x2234))+(((-0.55)*x2234))+((x2245*x2246)));
evalcond[3]=((0.045)+(((-1.0)*x2244))+(((-1.0)*x2249*x2250))+(((-1.0)*sj8*x2241))+x2247+x2246+x2248+(((-1.0)*x2250*x2251)));
evalcond[4]=((((0.55)*x2236))+(((-1.0)*x2249))+((x2233*x2236))+(((-1.0)*x2244*x2250))+((x2246*x2250))+(((-1.0)*x2251))+((x2232*x2236))+((x2234*x2243)));
evalcond[5]=((-0.2125)+(((-0.09)*x2247))+(((-0.09)*x2248))+((x2238*x2252))+(((1.1)*x2239))+(((1.1)*x2237))+(((1.1)*x2235))+((cj4*px*x2252))+(((-1.0)*(1.0)*pp))+(((0.09)*py*x2242)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}

} else
{
{
IkReal j6array[1], cj6array[1], sj6array[1];
bool j6valid[1]={false};
_nj6 = 1;
IkReal x2253=(cj4*px);
IkReal x2254=((0.045)*pz);
IkReal x2255=(py*sj4);
IkReal x2256=((0.3)*cj9);
IkReal x2257=((0.045)*sj9);
IkReal x2258=(cj8*cj9);
IkReal x2259=(cj8*sj9);
IkReal x2260=cj9*cj9;
IkReal x2261=((1.0)*pz);
CheckValue<IkReal> x2262 = IKatan2WithCheck(IkReal(((-0.304525)+(((-1.0)*(0.027)*cj9*sj9))+(((-1.0)*(0.0495)*sj9))+(pz*pz)+(((-0.087975)*x2260))+(((-1.0)*(0.33)*cj9)))),((((-1.0)*x2255*x2261))+(((0.027)*cj8*x2260))+(((0.01125)*x2258))+(((-1.0)*x2253*x2261))+(((-1.0)*(0.03825)*cj8))+(((-0.087975)*sj9*x2258))+(((-0.167025)*x2259))),IKFAST_ATAN2_MAGTHRESH);
if(!x2262.valid){
continue;
}
CheckValue<IkReal> x2263=IKPowWithIntegerCheck(IKsign(((((-1.0)*x2255*x2257))+(((-1.0)*x2255*x2256))+((x2254*x2258))+(((-1.0)*x2253*x2257))+(((-0.55)*x2255))+(((-0.3)*pz*x2259))+(((-1.0)*x2253*x2256))+(((-0.55)*x2253))+(((-1.0)*cj8*x2254)))),-1);
if(!x2263.valid){
continue;
}
j6array[0]=((-1.5707963267949)+(x2262.value)+(((1.5707963267949)*(x2263.value))));
sj6array[0]=IKsin(j6array[0]);
cj6array[0]=IKcos(j6array[0]);
if( j6array[0] > IKPI )
{
    j6array[0]-=IK2PI;
}
else if( j6array[0] < -IKPI )
{    j6array[0]+=IK2PI;
}
j6valid[0] = true;
for(int ij6 = 0; ij6 < 1; ++ij6)
{
if( !j6valid[ij6] )
{
    continue;
}
_ij6[0] = ij6; _ij6[1] = -1;
for(int iij6 = ij6+1; iij6 < 1; ++iij6)
{
if( j6valid[iij6] && IKabs(cj6array[ij6]-cj6array[iij6]) < IKFAST_SOLUTION_THRESH && IKabs(sj6array[ij6]-sj6array[iij6]) < IKFAST_SOLUTION_THRESH )
{
    j6valid[iij6]=false; _ij6[1] = iij6; break;
}
}
j6 = j6array[ij6]; cj6 = cj6array[ij6]; sj6 = sj6array[ij6];
{
IkReal evalcond[6];
IkReal x2264=((0.3)*cj9);
IkReal x2265=((0.045)*sj9);
IkReal x2266=IKcos(j6);
IkReal x2267=(pz*x2266);
IkReal x2268=IKsin(j6);
IkReal x2269=(cj4*px*x2268);
IkReal x2270=(py*sj4);
IkReal x2271=(x2268*x2270);
IkReal x2272=(px*sj4);
IkReal x2273=((1.0)*cj4*py);
IkReal x2274=(cj4*sj8);
IkReal x2275=((0.045)*cj8);
IkReal x2276=((0.045)*cj9);
IkReal x2277=(cj8*x2268);
IkReal x2278=((0.3)*sj9);
IkReal x2279=(sj8*x2272);
IkReal x2280=(pz*x2277);
IkReal x2281=(px*(((1.0)*cj4)));
IkReal x2282=(cj8*x2266);
IkReal x2283=((1.0)*x2270);
IkReal x2284=((0.09)*cj8*x2266);
evalcond[0]=((-0.55)+x2267+x2269+(((-1.0)*x2265))+x2271+(((-1.0)*x2264)));
evalcond[1]=(((cj8*x2272))+(((-1.0)*cj8*x2273))+(((-1.0)*pz*sj8*x2268))+((sj8*x2266*x2270))+((px*x2266*x2274)));
evalcond[2]=((((-1.0)*x2264*x2266))+((x2268*x2275))+pz+(((-1.0)*x2265*x2266))+((x2277*x2278))+(((-0.55)*x2266))+(((-1.0)*x2276*x2277)));
evalcond[3]=((0.045)+(((-1.0)*x2282*x2283))+x2280+(((-1.0)*sj8*x2273))+(((-1.0)*x2281*x2282))+(((-1.0)*x2276))+x2278+x2279);
evalcond[4]=((((-1.0)*x2281))+((x2266*x2275))+(((-1.0)*x2276*x2282))+((x2264*x2268))+((x2278*x2282))+((x2265*x2268))+(((-1.0)*x2283))+(((0.55)*x2268)));
evalcond[5]=((-0.2125)+(((-0.09)*x2279))+(((-0.09)*x2280))+((cj4*px*x2284))+(((0.09)*py*x2274))+(((1.1)*x2267))+((x2270*x2284))+(((-1.0)*(1.0)*pp))+(((1.1)*x2269))+(((1.1)*x2271)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

rotationfunction0(solutions);
}
}

}

}
}
}

}

}
}
}
}
return solutions.GetNumSolutions()>0;
}
inline void rotationfunction0(IkSolutionListBase<IkReal>& solutions) {
for(int rotationiter = 0; rotationiter < 1; ++rotationiter) {
IkReal x158=((1.0)*sj9);
IkReal x159=(cj9*sj6);
IkReal x160=((((-1.0)*cj6*x158))+(((-1.0)*cj8*x159)));
IkReal x161=(cj4*sj8);
IkReal x162=(sj6*x158);
IkReal x163=(cj6*cj9);
IkReal x164=(((cj8*x163))+(((-1.0)*x162)));
IkReal x165=(((cj9*x161))+((sj4*x164)));
IkReal x166=((1.0)*sj4*sj8);
IkReal x167=((((-1.0)*cj9*x166))+((cj4*x164)));
IkReal x168=(sj6*sj8);
IkReal x169=(((cj4*cj8))+(((-1.0)*cj6*x166)));
IkReal x170=((((-1.0)*cj6*x161))+(((-1.0)*cj8*sj4)));
IkReal x171=((((-1.0)*cj8*x162))+x163);
IkReal x172=(x159+((cj6*cj8*sj9)));
IkReal x173=(((sj4*x172))+((sj9*x161)));
IkReal x174=(((cj4*x172))+(((-1.0)*sj4*sj8*x158)));
new_r00=(((r00*x167))+((r20*x160))+((r10*x165)));
new_r01=(((r11*x165))+((r21*x160))+((r01*x167)));
new_r02=(((r22*x160))+((r12*x165))+((r02*x167)));
new_r10=(((r20*x168))+((r10*x169))+((r00*x170)));
new_r11=(((r01*x170))+((r21*x168))+((r11*x169)));
new_r12=(((r22*x168))+((r02*x170))+((r12*x169)));
new_r20=(((r20*x171))+((r10*x173))+((r00*x174)));
new_r21=(((r01*x174))+((r11*x173))+((r21*x171)));
new_r22=(((r12*x173))+((r22*x171))+((r02*x174)));
{
IkReal j11array[2], cj11array[2], sj11array[2];
bool j11valid[2]={false};
_nj11 = 2;
cj11array[0]=new_r22;
if( cj11array[0] >= -1-IKFAST_SINCOS_THRESH && cj11array[0] <= 1+IKFAST_SINCOS_THRESH )
{
    j11valid[0] = j11valid[1] = true;
    j11array[0] = IKacos(cj11array[0]);
    sj11array[0] = IKsin(j11array[0]);
    cj11array[1] = cj11array[0];
    j11array[1] = -j11array[0];
    sj11array[1] = -sj11array[0];
}
else if( std::isnan(cj11array[0]) )
{
    // probably any value will work
    j11valid[0] = true;
    cj11array[0] = 1; sj11array[0] = 0; j11array[0] = 0;
}
for(int ij11 = 0; ij11 < 2; ++ij11)
{
if( !j11valid[ij11] )
{
    continue;
}
_ij11[0] = ij11; _ij11[1] = -1;
for(int iij11 = ij11+1; iij11 < 2; ++iij11)
{
if( j11valid[iij11] && IKabs(cj11array[ij11]-cj11array[iij11]) < IKFAST_SOLUTION_THRESH && IKabs(sj11array[ij11]-sj11array[iij11]) < IKFAST_SOLUTION_THRESH )
{
    j11valid[iij11]=false; _ij11[1] = iij11; break;
}
}
j11 = j11array[ij11]; cj11 = cj11array[ij11]; sj11 = sj11array[ij11];

{
IkReal j10eval[2];
IkReal x175=((1.0)*sj9);
IkReal x176=(cj9*sj6);
IkReal x177=x160;
IkReal x178=(cj4*sj8);
IkReal x179=(sj6*x175);
IkReal x180=(cj6*cj9);
IkReal x181=(((cj8*x180))+(((-1.0)*x179)));
IkReal x182=(((sj4*x181))+((cj9*x178)));
IkReal x183=((1.0)*sj4*sj8);
IkReal x184=(((cj4*x181))+(((-1.0)*cj9*x183)));
IkReal x185=(sj6*sj8);
IkReal x186=x169;
IkReal x187=x170;
IkReal x188=((((-1.0)*cj8*x179))+x180);
IkReal x189=x172;
IkReal x190=(((sj9*x178))+((sj4*x189)));
IkReal x191=((((-1.0)*sj4*sj8*x175))+((cj4*x189)));
new_r00=(((r20*x177))+((r00*x184))+((r10*x182)));
new_r01=(((r21*x177))+((r11*x182))+((r01*x184)));
new_r02=(((r22*x177))+((r12*x182))+((r02*x184)));
new_r10=(((r10*x186))+((r20*x185))+((r00*x187)));
new_r11=(((r21*x185))+((r11*x186))+((r01*x187)));
new_r12=(((r12*x186))+((r22*x185))+((r02*x187)));
new_r20=(((r00*x191))+((r20*x188))+((r10*x190)));
new_r21=(((r21*x188))+((r11*x190))+((r01*x191)));
new_r22=(((r12*x190))+((r22*x188))+((r02*x191)));
j10eval[0]=sj11;
j10eval[1]=IKsign(sj11);
if( IKabs(j10eval[0]) < 0.0000010000000000  || IKabs(j10eval[1]) < 0.0000010000000000  )
{
{
IkReal j10eval[1];
IkReal x192=((1.0)*sj9);
IkReal x193=(cj9*sj6);
IkReal x194=x160;
IkReal x195=(cj4*sj8);
IkReal x196=(sj6*x192);
IkReal x197=(cj6*cj9);
IkReal x198=(((cj8*x197))+(((-1.0)*x196)));
IkReal x199=(((cj9*x195))+((sj4*x198)));
IkReal x200=((1.0)*sj4*sj8);
IkReal x201=((((-1.0)*cj9*x200))+((cj4*x198)));
IkReal x202=(sj6*sj8);
IkReal x203=x169;
IkReal x204=x170;
IkReal x205=((((-1.0)*cj8*x196))+x197);
IkReal x206=x172;
IkReal x207=(((sj9*x195))+((sj4*x206)));
IkReal x208=(((cj4*x206))+(((-1.0)*sj4*sj8*x192)));
new_r00=(((r20*x194))+((r00*x201))+((r10*x199)));
new_r01=(((r11*x199))+((r01*x201))+((r21*x194)));
new_r02=(((r02*x201))+((r22*x194))+((r12*x199)));
new_r10=(((r00*x204))+((r10*x203))+((r20*x202)));
new_r11=(((r21*x202))+((r11*x203))+((r01*x204)));
new_r12=(((r12*x203))+((r22*x202))+((r02*x204)));
new_r20=(((r20*x205))+((r10*x207))+((r00*x208)));
new_r21=(((r11*x207))+((r01*x208))+((r21*x205)));
new_r22=(((r12*x207))+((r02*x208))+((r22*x205)));
j10eval[0]=sj11;
if( IKabs(j10eval[0]) < 0.0000010000000000  )
{
{
IkReal evalcond[6];
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j11))), 6.28318530717959)));
evalcond[1]=((-1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j10array[2], cj10array[2], sj10array[2];
bool j10valid[2]={false};
_nj10 = 2;
CheckValue<IkReal> x210 = IKatan2WithCheck(IkReal(new_r02),new_r12,IKFAST_ATAN2_MAGTHRESH);
if(!x210.valid){
continue;
}
IkReal x209=((-1.0)*(((1.0)*(x210.value))));
j10array[0]=x209;
sj10array[0]=IKsin(j10array[0]);
cj10array[0]=IKcos(j10array[0]);
j10array[1]=((3.14159265358979)+x209);
sj10array[1]=IKsin(j10array[1]);
cj10array[1]=IKcos(j10array[1]);
if( j10array[0] > IKPI )
{
    j10array[0]-=IK2PI;
}
else if( j10array[0] < -IKPI )
{    j10array[0]+=IK2PI;
}
j10valid[0] = true;
if( j10array[1] > IKPI )
{
    j10array[1]-=IK2PI;
}
else if( j10array[1] < -IKPI )
{    j10array[1]+=IK2PI;
}
j10valid[1] = true;
for(int ij10 = 0; ij10 < 2; ++ij10)
{
if( !j10valid[ij10] )
{
    continue;
}
_ij10[0] = ij10; _ij10[1] = -1;
for(int iij10 = ij10+1; iij10 < 2; ++iij10)
{
if( j10valid[iij10] && IKabs(cj10array[ij10]-cj10array[iij10]) < IKFAST_SOLUTION_THRESH && IKabs(sj10array[ij10]-sj10array[iij10]) < IKFAST_SOLUTION_THRESH )
{
    j10valid[iij10]=false; _ij10[1] = iij10; break;
}
}
j10 = j10array[ij10]; cj10 = cj10array[ij10]; sj10 = sj10array[ij10];
{
IkReal evalcond[1];
evalcond[0]=(((new_r12*(IKcos(j10))))+(((-1.0)*(1.0)*new_r02*(IKsin(j10)))));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x211=((1.0)*new_r01);
if( IKabs(((((-1.0)*cj10*x211))+(((-1.0)*(1.0)*new_r00*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((((-1.0)*sj10*x211))+((cj10*new_r00)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((-1.0)*cj10*x211))+(((-1.0)*(1.0)*new_r00*sj10))))+IKsqr(((((-1.0)*sj10*x211))+((cj10*new_r00))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((((-1.0)*cj10*x211))+(((-1.0)*(1.0)*new_r00*sj10))), ((((-1.0)*sj10*x211))+((cj10*new_r00))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x212=IKsin(j12);
IkReal x213=(cj10*x212);
IkReal x214=IKcos(j12);
IkReal x215=((1.0)*x214);
IkReal x216=((-1.0)*x215);
IkReal x217=((1.0)*sj10);
IkReal x218=((((-1.0)*cj10*x215))+((sj10*x212)));
evalcond[0]=(((cj10*new_r01))+x212+((new_r11*sj10)));
evalcond[1]=(((sj10*x214))+x213+new_r01);
evalcond[2]=(((cj10*new_r00))+((new_r10*sj10))+x216);
evalcond[3]=((((-1.0)*new_r00*x217))+(((-1.0)*x212))+((cj10*new_r10)));
evalcond[4]=(((cj10*new_r11))+x216+(((-1.0)*new_r01*x217)));
evalcond[5]=(x218+new_r00);
evalcond[6]=(x218+new_r11);
evalcond[7]=((((-1.0)*x213))+(((-1.0)*x214*x217))+new_r10);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j11)))), 6.28318530717959)));
evalcond[1]=((1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j10array[2], cj10array[2], sj10array[2];
bool j10valid[2]={false};
_nj10 = 2;
CheckValue<IkReal> x220 = IKatan2WithCheck(IkReal(new_r02),new_r12,IKFAST_ATAN2_MAGTHRESH);
if(!x220.valid){
continue;
}
IkReal x219=((-1.0)*(((1.0)*(x220.value))));
j10array[0]=x219;
sj10array[0]=IKsin(j10array[0]);
cj10array[0]=IKcos(j10array[0]);
j10array[1]=((3.14159265358979)+x219);
sj10array[1]=IKsin(j10array[1]);
cj10array[1]=IKcos(j10array[1]);
if( j10array[0] > IKPI )
{
    j10array[0]-=IK2PI;
}
else if( j10array[0] < -IKPI )
{    j10array[0]+=IK2PI;
}
j10valid[0] = true;
if( j10array[1] > IKPI )
{
    j10array[1]-=IK2PI;
}
else if( j10array[1] < -IKPI )
{    j10array[1]+=IK2PI;
}
j10valid[1] = true;
for(int ij10 = 0; ij10 < 2; ++ij10)
{
if( !j10valid[ij10] )
{
    continue;
}
_ij10[0] = ij10; _ij10[1] = -1;
for(int iij10 = ij10+1; iij10 < 2; ++iij10)
{
if( j10valid[iij10] && IKabs(cj10array[ij10]-cj10array[iij10]) < IKFAST_SOLUTION_THRESH && IKabs(sj10array[ij10]-sj10array[iij10]) < IKFAST_SOLUTION_THRESH )
{
    j10valid[iij10]=false; _ij10[1] = iij10; break;
}
}
j10 = j10array[ij10]; cj10 = cj10array[ij10]; sj10 = sj10array[ij10];
{
IkReal evalcond[1];
evalcond[0]=(((new_r12*(IKcos(j10))))+(((-1.0)*(1.0)*new_r02*(IKsin(j10)))));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x221=((1.0)*new_r00);
if( IKabs(((((-1.0)*sj10*x221))+((cj10*new_r01)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((((-1.0)*cj10*x221))+(((-1.0)*(1.0)*new_r01*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((-1.0)*sj10*x221))+((cj10*new_r01))))+IKsqr(((((-1.0)*cj10*x221))+(((-1.0)*(1.0)*new_r01*sj10))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((((-1.0)*sj10*x221))+((cj10*new_r01))), ((((-1.0)*cj10*x221))+(((-1.0)*(1.0)*new_r01*sj10))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x222=IKcos(j12);
IkReal x223=IKsin(j12);
IkReal x224=((1.0)*x223);
IkReal x225=((-1.0)*x224);
IkReal x226=(cj10*x222);
IkReal x227=((1.0)*sj10);
IkReal x228=(((sj10*x222))+(((-1.0)*cj10*x224)));
evalcond[0]=(((cj10*new_r00))+((new_r10*sj10))+x222);
evalcond[1]=(((cj10*new_r01))+((new_r11*sj10))+x225);
evalcond[2]=(((sj10*x223))+new_r00+x226);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*new_r00*x227))+x225);
evalcond[4]=(((cj10*new_r11))+(((-1.0)*x222))+(((-1.0)*new_r01*x227)));
evalcond[5]=(new_r01+x228);
evalcond[6]=(new_r10+x228);
evalcond[7]=((((-1.0)*x226))+new_r11+(((-1.0)*x223*x227)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j10, j12]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}

} else
{
{
IkReal j10array[1], cj10array[1], sj10array[1];
bool j10valid[1]={false};
_nj10 = 1;
CheckValue<IkReal> x230=IKPowWithIntegerCheck(sj11,-1);
if(!x230.valid){
continue;
}
IkReal x229=x230.value;
CheckValue<IkReal> x231=IKPowWithIntegerCheck(new_r12,-1);
if(!x231.valid){
continue;
}
if( IKabs((x229*(x231.value)*(((1.0)+(((-1.0)*(1.0)*(cj11*cj11)))+(((-1.0)*(1.0)*(new_r02*new_r02))))))) < IKFAST_ATAN2_MAGTHRESH && IKabs((new_r02*x229)) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((x229*(x231.value)*(((1.0)+(((-1.0)*(1.0)*(cj11*cj11)))+(((-1.0)*(1.0)*(new_r02*new_r02)))))))+IKsqr((new_r02*x229))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j10array[0]=IKatan2((x229*(x231.value)*(((1.0)+(((-1.0)*(1.0)*(cj11*cj11)))+(((-1.0)*(1.0)*(new_r02*new_r02)))))), (new_r02*x229));
sj10array[0]=IKsin(j10array[0]);
cj10array[0]=IKcos(j10array[0]);
if( j10array[0] > IKPI )
{
    j10array[0]-=IK2PI;
}
else if( j10array[0] < -IKPI )
{    j10array[0]+=IK2PI;
}
j10valid[0] = true;
for(int ij10 = 0; ij10 < 1; ++ij10)
{
if( !j10valid[ij10] )
{
    continue;
}
_ij10[0] = ij10; _ij10[1] = -1;
for(int iij10 = ij10+1; iij10 < 1; ++iij10)
{
if( j10valid[iij10] && IKabs(cj10array[ij10]-cj10array[iij10]) < IKFAST_SOLUTION_THRESH && IKabs(sj10array[ij10]-sj10array[iij10]) < IKFAST_SOLUTION_THRESH )
{
    j10valid[iij10]=false; _ij10[1] = iij10; break;
}
}
j10 = j10array[ij10]; cj10 = cj10array[ij10]; sj10 = sj10array[ij10];
{
IkReal evalcond[8];
IkReal x232=IKcos(j10);
IkReal x233=((1.0)*sj11);
IkReal x234=(x232*x233);
IkReal x235=IKsin(j10);
IkReal x236=(x233*x235);
IkReal x237=(new_r02*x232);
IkReal x238=(new_r12*x235);
IkReal x239=((1.0)*cj11);
evalcond[0]=((((-1.0)*x234))+new_r02);
evalcond[1]=(new_r12+(((-1.0)*x236)));
evalcond[2]=(((new_r12*x232))+(((-1.0)*new_r02*x235)));
evalcond[3]=((((-1.0)*x233))+x237+x238);
evalcond[4]=(((cj11*x238))+(((-1.0)*new_r22*x233))+((cj11*x237)));
evalcond[5]=((((-1.0)*new_r00*x234))+(((-1.0)*new_r10*x236))+(((-1.0)*new_r20*x239)));
evalcond[6]=((((-1.0)*new_r01*x234))+(((-1.0)*new_r21*x239))+(((-1.0)*new_r11*x236)));
evalcond[7]=((1.0)+(((-1.0)*new_r22*x239))+(((-1.0)*x233*x238))+(((-1.0)*x233*x237)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
IkReal j12eval[2];
IkReal x240=((1.0)*sj9);
IkReal x241=(cj9*sj6);
IkReal x242=x160;
IkReal x243=(cj4*sj8);
IkReal x244=(sj6*x240);
IkReal x245=(cj6*cj9);
IkReal x246=(((cj8*x245))+(((-1.0)*x244)));
IkReal x247=(((cj9*x243))+((sj4*x246)));
IkReal x248=((1.0)*sj4*sj8);
IkReal x249=((((-1.0)*cj9*x248))+((cj4*x246)));
IkReal x250=(sj6*sj8);
IkReal x251=x169;
IkReal x252=x170;
IkReal x253=(x245+(((-1.0)*cj8*x244)));
IkReal x254=x172;
IkReal x255=(((sj9*x243))+((sj4*x254)));
IkReal x256=(((cj4*x254))+(((-1.0)*sj4*sj8*x240)));
new_r00=(((r20*x242))+((r00*x249))+((r10*x247)));
new_r01=(((r21*x242))+((r01*x249))+((r11*x247)));
new_r02=(((r22*x242))+((r12*x247))+((r02*x249)));
new_r10=(((r10*x251))+((r20*x250))+((r00*x252)));
new_r11=(((r11*x251))+((r21*x250))+((r01*x252)));
new_r12=(((r22*x250))+((r12*x251))+((r02*x252)));
new_r20=(((r20*x253))+((r00*x256))+((r10*x255)));
new_r21=(((r21*x253))+((r11*x255))+((r01*x256)));
new_r22=(((r02*x256))+((r12*x255))+((r22*x253)));
j12eval[0]=sj11;
j12eval[1]=IKsign(sj11);
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  )
{
{
IkReal j12eval[2];
IkReal x257=((1.0)*sj9);
IkReal x258=(cj9*sj6);
IkReal x259=x160;
IkReal x260=(cj4*sj8);
IkReal x261=(sj6*x257);
IkReal x262=(cj6*cj9);
IkReal x263=(((cj8*x262))+(((-1.0)*x261)));
IkReal x264=(((sj4*x263))+((cj9*x260)));
IkReal x265=((1.0)*sj4*sj8);
IkReal x266=((((-1.0)*cj9*x265))+((cj4*x263)));
IkReal x267=(sj6*sj8);
IkReal x268=x169;
IkReal x269=x170;
IkReal x270=(x262+(((-1.0)*cj8*x261)));
IkReal x271=x172;
IkReal x272=(((sj4*x271))+((sj9*x260)));
IkReal x273=(((cj4*x271))+(((-1.0)*sj4*sj8*x257)));
new_r00=(((r00*x266))+((r20*x259))+((r10*x264)));
new_r01=(((r21*x259))+((r01*x266))+((r11*x264)));
new_r02=(((r02*x266))+((r12*x264))+((r22*x259)));
new_r10=(((r20*x267))+((r00*x269))+((r10*x268)));
new_r11=(((r01*x269))+((r21*x267))+((r11*x268)));
new_r12=(((r22*x267))+((r12*x268))+((r02*x269)));
new_r20=(((r00*x273))+((r10*x272))+((r20*x270)));
new_r21=(((r11*x272))+((r01*x273))+((r21*x270)));
new_r22=(((r12*x272))+((r02*x273))+((r22*x270)));
j12eval[0]=sj10;
j12eval[1]=sj11;
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  )
{
{
IkReal j12eval[3];
IkReal x274=((1.0)*sj9);
IkReal x275=(cj9*sj6);
IkReal x276=x160;
IkReal x277=(cj4*sj8);
IkReal x278=(sj6*x274);
IkReal x279=(cj6*cj9);
IkReal x280=(((cj8*x279))+(((-1.0)*x278)));
IkReal x281=(((cj9*x277))+((sj4*x280)));
IkReal x282=((1.0)*sj4*sj8);
IkReal x283=(((cj4*x280))+(((-1.0)*cj9*x282)));
IkReal x284=(sj6*sj8);
IkReal x285=x169;
IkReal x286=x170;
IkReal x287=((((-1.0)*cj8*x278))+x279);
IkReal x288=x172;
IkReal x289=(((sj9*x277))+((sj4*x288)));
IkReal x290=(((cj4*x288))+(((-1.0)*sj4*sj8*x274)));
new_r00=(((r00*x283))+((r10*x281))+((r20*x276)));
new_r01=(((r01*x283))+((r11*x281))+((r21*x276)));
new_r02=(((r22*x276))+((r02*x283))+((r12*x281)));
new_r10=(((r10*x285))+((r00*x286))+((r20*x284)));
new_r11=(((r01*x286))+((r21*x284))+((r11*x285)));
new_r12=(((r22*x284))+((r02*x286))+((r12*x285)));
new_r20=(((r00*x290))+((r20*x287))+((r10*x289)));
new_r21=(((r21*x287))+((r11*x289))+((r01*x290)));
new_r22=(((r22*x287))+((r12*x289))+((r02*x290)));
j12eval[0]=cj10;
j12eval[1]=cj11;
j12eval[2]=sj11;
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  || IKabs(j12eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[12];
bool bgotonextstatement = true;
do
{
IkReal x291=((1.0)*cj11);
IkReal x292=((((-1.0)*x291))+new_r22);
IkReal x293=((1.0)*sj11);
IkReal x294=((((-1.0)*x293))+new_r12);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j10)))), 6.28318530717959)));
evalcond[1]=x292;
evalcond[2]=x292;
evalcond[3]=new_r02;
evalcond[4]=x294;
evalcond[5]=x294;
evalcond[6]=((((-1.0)*new_r22*x293))+((cj11*new_r12)));
evalcond[7]=((((-1.0)*new_r10*x293))+(((-1.0)*new_r20*x291)));
evalcond[8]=((((-1.0)*new_r11*x293))+(((-1.0)*new_r21*x291)));
evalcond[9]=((1.0)+(((-1.0)*new_r12*x293))+(((-1.0)*new_r22*x291)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x295=IKPowWithIntegerCheck(IKsign(new_r12),-1);
if(!x295.valid){
continue;
}
CheckValue<IkReal> x296 = IKatan2WithCheck(IkReal(new_r21),((-1.0)*(((1.0)*new_r20))),IKFAST_ATAN2_MAGTHRESH);
if(!x296.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(((1.5707963267949)*(x295.value)))+(x296.value));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x297=IKcos(j12);
IkReal x298=IKsin(j12);
IkReal x299=((1.0)*new_r12);
IkReal x300=((1.0)*x297);
IkReal x301=((-1.0)*x300);
evalcond[0]=(((new_r12*x297))+new_r20);
evalcond[1]=(((new_r22*x298))+new_r11);
evalcond[2]=((((-1.0)*x298*x299))+new_r21);
evalcond[3]=((((-1.0)*new_r22*x300))+new_r10);
evalcond[4]=((((-1.0)*(1.0)*new_r00))+(((-1.0)*x298)));
evalcond[5]=(x301+(((-1.0)*(1.0)*new_r01)));
evalcond[6]=(((new_r11*new_r22))+x298+(((-1.0)*new_r21*x299)));
evalcond[7]=((((-1.0)*new_r20*x299))+x301+((new_r10*new_r22)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x302=((1.0)*cj11);
IkReal x303=((((-1.0)*x302))+new_r22);
IkReal x304=((1.0)*sj11);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j10)))), 6.28318530717959)));
evalcond[1]=x303;
evalcond[2]=x303;
evalcond[3]=new_r02;
evalcond[4]=(sj11+new_r12);
evalcond[5]=((((-1.0)*(1.0)*new_r12))+(((-1.0)*x304)));
evalcond[6]=((((-1.0)*new_r22*x304))+(((-1.0)*new_r12*x302)));
evalcond[7]=(((new_r10*sj11))+(((-1.0)*new_r20*x302)));
evalcond[8]=((((-1.0)*new_r21*x302))+((new_r11*sj11)));
evalcond[9]=((1.0)+(((-1.0)*new_r22*x302))+((new_r12*sj11)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r00) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r01) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r00)+IKsqr(new_r01)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r00, new_r01);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x305=IKsin(j12);
IkReal x306=((1.0)*(IKcos(j12)));
IkReal x307=((-1.0)*x306);
IkReal x308=((1.0)*new_r11);
IkReal x309=((1.0)*new_r10);
evalcond[0]=(((new_r12*x305))+new_r21);
evalcond[1]=((((-1.0)*x305))+new_r00);
evalcond[2]=(x307+new_r01);
evalcond[3]=((((-1.0)*new_r12*x306))+new_r20);
evalcond[4]=((((-1.0)*x308))+((new_r22*x305)));
evalcond[5]=((((-1.0)*x309))+(((-1.0)*new_r22*x306)));
evalcond[6]=(x305+((new_r12*new_r21))+(((-1.0)*new_r22*x308)));
evalcond[7]=(x307+((new_r12*new_r20))+(((-1.0)*new_r22*x309)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x310=((1.0)*cj10);
IkReal x311=((1.0)*sj10);
IkReal x312=((((-1.0)*new_r02*x311))+((cj10*new_r12)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j11)))), 6.28318530717959)));
evalcond[1]=new_r22;
evalcond[2]=((((-1.0)*x310))+new_r02);
evalcond[3]=(new_r12+(((-1.0)*x311)));
evalcond[4]=x312;
evalcond[5]=x312;
evalcond[6]=((-1.0)+((cj10*new_r02))+((new_r12*sj10)));
evalcond[7]=(((cj10*new_r01))+((new_r11*sj10)));
evalcond[8]=(((cj10*new_r00))+((new_r10*sj10)));
evalcond[9]=((((-1.0)*new_r10*x311))+(((-1.0)*new_r00*x310)));
evalcond[10]=((((-1.0)*new_r01*x310))+(((-1.0)*new_r11*x311)));
evalcond[11]=((1.0)+(((-1.0)*new_r12*x311))+(((-1.0)*new_r02*x310)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  && IKabs(evalcond[10]) < 0.0000010000000000  && IKabs(evalcond[11]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r21) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*(((1.0)*new_r20)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r21)+IKsqr(((-1.0)*(((1.0)*new_r20))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r21, ((-1.0)*(((1.0)*new_r20))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x313=IKcos(j12);
IkReal x314=IKsin(j12);
IkReal x315=((1.0)*x314);
IkReal x316=((-1.0)*x315);
IkReal x317=((1.0)*x313);
IkReal x318=((1.0)*new_r12);
evalcond[0]=(x313+new_r20);
evalcond[1]=(x316+new_r21);
evalcond[2]=(((new_r12*x313))+new_r01);
evalcond[3]=(((new_r12*x314))+new_r00);
evalcond[4]=((((-1.0)*new_r02*x317))+new_r11);
evalcond[5]=(new_r10+(((-1.0)*new_r02*x315)));
evalcond[6]=((((-1.0)*new_r00*x318))+x316+((new_r02*new_r10)));
evalcond[7]=(((new_r02*new_r11))+(((-1.0)*new_r01*x318))+(((-1.0)*x317)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x319=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x320=((1.0)+((cj10*new_r02))+((new_r12*sj10)));
IkReal x321=(((cj10*new_r01))+((new_r11*sj10)));
IkReal x322=(((cj10*new_r00))+((new_r10*sj10)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j11)))), 6.28318530717959)));
evalcond[1]=new_r22;
evalcond[2]=(cj10+new_r02);
evalcond[3]=(sj10+new_r12);
evalcond[4]=x319;
evalcond[5]=x319;
evalcond[6]=x320;
evalcond[7]=x321;
evalcond[8]=x322;
evalcond[9]=x322;
evalcond[10]=x321;
evalcond[11]=x320;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  && IKabs(evalcond[10]) < 0.0000010000000000  && IKabs(evalcond[11]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(((-1.0)*(((1.0)*new_r21)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r20) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((-1.0)*(((1.0)*new_r21))))+IKsqr(new_r20)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((-1.0)*(((1.0)*new_r21))), new_r20);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x323=IKsin(j12);
IkReal x324=IKcos(j12);
IkReal x325=((1.0)*x324);
IkReal x326=((-1.0)*x325);
IkReal x327=((1.0)*x323);
IkReal x328=((1.0)*new_r02);
evalcond[0]=(x323+new_r21);
evalcond[1]=(x326+new_r20);
evalcond[2]=(((new_r02*x324))+new_r11);
evalcond[3]=(((new_r02*x323))+new_r10);
evalcond[4]=((((-1.0)*new_r12*x325))+new_r01);
evalcond[5]=((((-1.0)*new_r12*x327))+new_r00);
evalcond[6]=(((new_r00*new_r12))+(((-1.0)*x327))+(((-1.0)*new_r10*x328)));
evalcond[7]=(((new_r01*new_r12))+x326+(((-1.0)*new_r11*x328)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x329=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x330=(((cj10*new_r02))+((new_r12*sj10)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j11))), 6.28318530717959)));
evalcond[1]=((-1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
evalcond[6]=x329;
evalcond[7]=x329;
evalcond[8]=x330;
evalcond[9]=x330;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x331=((1.0)*new_r01);
if( IKabs(((((-1.0)*cj10*x331))+(((-1.0)*(1.0)*new_r00*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs((((cj10*new_r00))+(((-1.0)*sj10*x331)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((-1.0)*cj10*x331))+(((-1.0)*(1.0)*new_r00*sj10))))+IKsqr((((cj10*new_r00))+(((-1.0)*sj10*x331))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((((-1.0)*cj10*x331))+(((-1.0)*(1.0)*new_r00*sj10))), (((cj10*new_r00))+(((-1.0)*sj10*x331))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x332=IKsin(j12);
IkReal x333=(cj10*x332);
IkReal x334=IKcos(j12);
IkReal x335=((1.0)*x334);
IkReal x336=((-1.0)*x335);
IkReal x337=((1.0)*sj10);
IkReal x338=(((sj10*x332))+(((-1.0)*cj10*x335)));
evalcond[0]=(((cj10*new_r01))+x332+((new_r11*sj10)));
evalcond[1]=(((sj10*x334))+x333+new_r01);
evalcond[2]=(((cj10*new_r00))+((new_r10*sj10))+x336);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x332))+(((-1.0)*new_r00*x337)));
evalcond[4]=(((cj10*new_r11))+x336+(((-1.0)*new_r01*x337)));
evalcond[5]=(x338+new_r00);
evalcond[6]=(x338+new_r11);
evalcond[7]=((((-1.0)*x334*x337))+(((-1.0)*x333))+new_r10);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x339=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x340=(cj10*new_r02);
IkReal x341=(new_r12*sj10);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j11)))), 6.28318530717959)));
evalcond[1]=((1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
evalcond[6]=x339;
evalcond[7]=x339;
evalcond[8]=(x340+x341);
evalcond[9]=((((-1.0)*x341))+(((-1.0)*x340)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x342=((1.0)*new_r00);
if( IKabs((((cj10*new_r01))+(((-1.0)*sj10*x342)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((((-1.0)*cj10*x342))+(((-1.0)*(1.0)*new_r01*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((((cj10*new_r01))+(((-1.0)*sj10*x342))))+IKsqr(((((-1.0)*cj10*x342))+(((-1.0)*(1.0)*new_r01*sj10))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2((((cj10*new_r01))+(((-1.0)*sj10*x342))), ((((-1.0)*cj10*x342))+(((-1.0)*(1.0)*new_r01*sj10))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x343=IKcos(j12);
IkReal x344=IKsin(j12);
IkReal x345=((1.0)*x344);
IkReal x346=((-1.0)*x345);
IkReal x347=(cj10*x343);
IkReal x348=((1.0)*sj10);
IkReal x349=(((sj10*x343))+(((-1.0)*cj10*x345)));
evalcond[0]=(((cj10*new_r00))+((new_r10*sj10))+x343);
evalcond[1]=(((cj10*new_r01))+((new_r11*sj10))+x346);
evalcond[2]=(((sj10*x344))+x347+new_r00);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*new_r00*x348))+x346);
evalcond[4]=(((cj10*new_r11))+(((-1.0)*new_r01*x348))+(((-1.0)*x343)));
evalcond[5]=(x349+new_r01);
evalcond[6]=(new_r10+x349);
evalcond[7]=((((-1.0)*x347))+(((-1.0)*x344*x348))+new_r11);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x350=((1.0)*cj11);
IkReal x351=((((-1.0)*x350))+new_r22);
IkReal x352=((1.0)*sj11);
IkReal x353=((((-1.0)*x352))+new_r02);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j10))), 6.28318530717959)));
evalcond[1]=x351;
evalcond[2]=x351;
evalcond[3]=x353;
evalcond[4]=new_r12;
evalcond[5]=x353;
evalcond[6]=((((-1.0)*new_r22*x352))+((cj11*new_r02)));
evalcond[7]=((((-1.0)*new_r00*x352))+(((-1.0)*new_r20*x350)));
evalcond[8]=((((-1.0)*new_r01*x352))+(((-1.0)*new_r21*x350)));
evalcond[9]=((1.0)+(((-1.0)*new_r02*x352))+(((-1.0)*new_r22*x350)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r10) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r11) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r10)+IKsqr(new_r11)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r10, new_r11);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x354=IKcos(j12);
IkReal x355=IKsin(j12);
IkReal x356=((1.0)*x354);
IkReal x357=((-1.0)*x356);
IkReal x358=((1.0)*new_r02);
evalcond[0]=(((new_r02*x354))+new_r20);
evalcond[1]=((((-1.0)*x355))+new_r10);
evalcond[2]=(x357+new_r11);
evalcond[3]=(((new_r22*x355))+new_r01);
evalcond[4]=((((-1.0)*x355*x358))+new_r21);
evalcond[5]=((((-1.0)*new_r22*x356))+new_r00);
evalcond[6]=(x355+((new_r01*new_r22))+(((-1.0)*new_r21*x358)));
evalcond[7]=(x357+((new_r00*new_r22))+(((-1.0)*new_r20*x358)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x359=((1.0)*cj11);
IkReal x360=((((-1.0)*x359))+new_r22);
IkReal x361=((1.0)*sj11);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j10)))), 6.28318530717959)));
evalcond[1]=x360;
evalcond[2]=x360;
evalcond[3]=(sj11+new_r02);
evalcond[4]=new_r12;
evalcond[5]=((((-1.0)*(1.0)*new_r02))+(((-1.0)*x361)));
evalcond[6]=((((-1.0)*new_r02*x359))+(((-1.0)*new_r22*x361)));
evalcond[7]=((((-1.0)*new_r20*x359))+((new_r00*sj11)));
evalcond[8]=(((new_r01*sj11))+(((-1.0)*new_r21*x359)));
evalcond[9]=((1.0)+(((-1.0)*new_r22*x359))+((new_r02*sj11)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x362 = IKatan2WithCheck(IkReal(((-1.0)*(((1.0)*new_r21)))),new_r20,IKFAST_ATAN2_MAGTHRESH);
if(!x362.valid){
continue;
}
CheckValue<IkReal> x363=IKPowWithIntegerCheck(IKsign(new_r02),-1);
if(!x363.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(x362.value)+(((1.5707963267949)*(x363.value))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x364=IKsin(j12);
IkReal x365=((1.0)*(IKcos(j12)));
IkReal x366=((-1.0)*x365);
IkReal x367=((1.0)*new_r01);
IkReal x368=((1.0)*new_r00);
evalcond[0]=(((new_r02*x364))+new_r21);
evalcond[1]=((((-1.0)*new_r02*x365))+new_r20);
evalcond[2]=((((-1.0)*x364))+(((-1.0)*(1.0)*new_r10)));
evalcond[3]=(x366+(((-1.0)*(1.0)*new_r11)));
evalcond[4]=((((-1.0)*x367))+((new_r22*x364)));
evalcond[5]=((((-1.0)*new_r22*x365))+(((-1.0)*x368)));
evalcond[6]=(((new_r02*new_r21))+x364+(((-1.0)*new_r22*x367)));
evalcond[7]=(((new_r02*new_r20))+x366+(((-1.0)*new_r22*x368)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j12]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}
}
}
}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x370=IKPowWithIntegerCheck(sj11,-1);
if(!x370.valid){
continue;
}
IkReal x369=x370.value;
CheckValue<IkReal> x371=IKPowWithIntegerCheck(cj10,-1);
if(!x371.valid){
continue;
}
CheckValue<IkReal> x372=IKPowWithIntegerCheck(cj11,-1);
if(!x372.valid){
continue;
}
if( IKabs((x369*(x371.value)*(x372.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*new_r20*x369)) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((x369*(x371.value)*(x372.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10))))))+IKsqr(((-1.0)*new_r20*x369))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2((x369*(x371.value)*(x372.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10))))), ((-1.0)*new_r20*x369));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x373=IKcos(j12);
IkReal x374=IKsin(j12);
IkReal x375=((1.0)*sj11);
IkReal x376=(cj10*new_r01);
IkReal x377=(new_r11*sj10);
IkReal x378=(cj11*x374);
IkReal x379=((1.0)*sj10);
IkReal x380=((1.0)*x374);
IkReal x381=((1.0)*x373);
IkReal x382=((-1.0)*x381);
IkReal x383=(cj10*new_r00);
IkReal x384=(new_r10*sj10);
IkReal x385=(cj10*x381);
evalcond[0]=(((sj11*x373))+new_r20);
evalcond[1]=((((-1.0)*x374*x375))+new_r21);
evalcond[2]=(x378+x377+x376);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x380))+(((-1.0)*new_r00*x379)));
evalcond[4]=(((cj10*new_r11))+x382+(((-1.0)*new_r01*x379)));
evalcond[5]=(((sj10*x373))+((cj10*x378))+new_r01);
evalcond[6]=((((-1.0)*cj11*x381))+x384+x383);
evalcond[7]=(((sj10*x374))+(((-1.0)*cj11*x385))+new_r00);
evalcond[8]=((((-1.0)*x385))+new_r11+((sj10*x378)));
evalcond[9]=((((-1.0)*cj10*x380))+(((-1.0)*cj11*x373*x379))+new_r10);
evalcond[10]=(((cj11*x376))+((cj11*x377))+(((-1.0)*new_r21*x375))+x374);
evalcond[11]=((((-1.0)*new_r20*x375))+x382+((cj11*x384))+((cj11*x383)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x388=IKPowWithIntegerCheck(sj11,-1);
if(!x388.valid){
continue;
}
IkReal x386=x388.value;
IkReal x387=((1.0)*new_r20);
CheckValue<IkReal> x389=IKPowWithIntegerCheck(sj10,-1);
if(!x389.valid){
continue;
}
if( IKabs((x386*(x389.value)*(((((-1.0)*cj10*cj11*x387))+(((-1.0)*(1.0)*new_r00*sj11)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*x386*x387)) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((x386*(x389.value)*(((((-1.0)*cj10*cj11*x387))+(((-1.0)*(1.0)*new_r00*sj11))))))+IKsqr(((-1.0)*x386*x387))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2((x386*(x389.value)*(((((-1.0)*cj10*cj11*x387))+(((-1.0)*(1.0)*new_r00*sj11))))), ((-1.0)*x386*x387));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x390=IKcos(j12);
IkReal x391=IKsin(j12);
IkReal x392=((1.0)*sj11);
IkReal x393=(cj10*new_r01);
IkReal x394=(new_r11*sj10);
IkReal x395=(cj11*x391);
IkReal x396=((1.0)*sj10);
IkReal x397=((1.0)*x391);
IkReal x398=((1.0)*x390);
IkReal x399=((-1.0)*x398);
IkReal x400=(cj10*new_r00);
IkReal x401=(new_r10*sj10);
IkReal x402=(cj10*x398);
evalcond[0]=(((sj11*x390))+new_r20);
evalcond[1]=((((-1.0)*x391*x392))+new_r21);
evalcond[2]=(x394+x395+x393);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x397))+(((-1.0)*new_r00*x396)));
evalcond[4]=(((cj10*new_r11))+x399+(((-1.0)*new_r01*x396)));
evalcond[5]=(((cj10*x395))+((sj10*x390))+new_r01);
evalcond[6]=(x401+x400+(((-1.0)*cj11*x398)));
evalcond[7]=(new_r00+(((-1.0)*cj11*x402))+((sj10*x391)));
evalcond[8]=((((-1.0)*x402))+((sj10*x395))+new_r11);
evalcond[9]=((((-1.0)*cj10*x397))+new_r10+(((-1.0)*cj11*x390*x396)));
evalcond[10]=(x391+((cj11*x393))+((cj11*x394))+(((-1.0)*new_r21*x392)));
evalcond[11]=(x399+((cj11*x400))+(((-1.0)*new_r20*x392))+((cj11*x401)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x403=IKPowWithIntegerCheck(IKsign(sj11),-1);
if(!x403.valid){
continue;
}
CheckValue<IkReal> x404 = IKatan2WithCheck(IkReal(new_r21),((-1.0)*(((1.0)*new_r20))),IKFAST_ATAN2_MAGTHRESH);
if(!x404.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(((1.5707963267949)*(x403.value)))+(x404.value));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x405=IKcos(j12);
IkReal x406=IKsin(j12);
IkReal x407=((1.0)*sj11);
IkReal x408=(cj10*new_r01);
IkReal x409=(new_r11*sj10);
IkReal x410=(cj11*x406);
IkReal x411=((1.0)*sj10);
IkReal x412=((1.0)*x406);
IkReal x413=((1.0)*x405);
IkReal x414=((-1.0)*x413);
IkReal x415=(cj10*new_r00);
IkReal x416=(new_r10*sj10);
IkReal x417=(cj10*x413);
evalcond[0]=(((sj11*x405))+new_r20);
evalcond[1]=((((-1.0)*x406*x407))+new_r21);
evalcond[2]=(x410+x409+x408);
evalcond[3]=((((-1.0)*new_r00*x411))+((cj10*new_r10))+(((-1.0)*x412)));
evalcond[4]=(((cj10*new_r11))+x414+(((-1.0)*new_r01*x411)));
evalcond[5]=(((cj10*x410))+((sj10*x405))+new_r01);
evalcond[6]=((((-1.0)*cj11*x413))+x415+x416);
evalcond[7]=((((-1.0)*cj11*x417))+((sj10*x406))+new_r00);
evalcond[8]=(((sj10*x410))+(((-1.0)*x417))+new_r11);
evalcond[9]=((((-1.0)*cj10*x412))+(((-1.0)*cj11*x405*x411))+new_r10);
evalcond[10]=(((cj11*x409))+(((-1.0)*new_r21*x407))+x406+((cj11*x408)));
evalcond[11]=(x414+((cj11*x416))+((cj11*x415))+(((-1.0)*new_r20*x407)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}
}
}

}

}

} else
{
{
IkReal j10array[1], cj10array[1], sj10array[1];
bool j10valid[1]={false};
_nj10 = 1;
CheckValue<IkReal> x418 = IKatan2WithCheck(IkReal(new_r12),new_r02,IKFAST_ATAN2_MAGTHRESH);
if(!x418.valid){
continue;
}
CheckValue<IkReal> x419=IKPowWithIntegerCheck(IKsign(sj11),-1);
if(!x419.valid){
continue;
}
j10array[0]=((-1.5707963267949)+(x418.value)+(((1.5707963267949)*(x419.value))));
sj10array[0]=IKsin(j10array[0]);
cj10array[0]=IKcos(j10array[0]);
if( j10array[0] > IKPI )
{
    j10array[0]-=IK2PI;
}
else if( j10array[0] < -IKPI )
{    j10array[0]+=IK2PI;
}
j10valid[0] = true;
for(int ij10 = 0; ij10 < 1; ++ij10)
{
if( !j10valid[ij10] )
{
    continue;
}
_ij10[0] = ij10; _ij10[1] = -1;
for(int iij10 = ij10+1; iij10 < 1; ++iij10)
{
if( j10valid[iij10] && IKabs(cj10array[ij10]-cj10array[iij10]) < IKFAST_SOLUTION_THRESH && IKabs(sj10array[ij10]-sj10array[iij10]) < IKFAST_SOLUTION_THRESH )
{
    j10valid[iij10]=false; _ij10[1] = iij10; break;
}
}
j10 = j10array[ij10]; cj10 = cj10array[ij10]; sj10 = sj10array[ij10];
{
IkReal evalcond[8];
IkReal x420=IKcos(j10);
IkReal x421=((1.0)*sj11);
IkReal x422=(x420*x421);
IkReal x423=IKsin(j10);
IkReal x424=(x421*x423);
IkReal x425=(new_r02*x420);
IkReal x426=(new_r12*x423);
IkReal x427=((1.0)*cj11);
evalcond[0]=((((-1.0)*x422))+new_r02);
evalcond[1]=((((-1.0)*x424))+new_r12);
evalcond[2]=((((-1.0)*new_r02*x423))+((new_r12*x420)));
evalcond[3]=((((-1.0)*x421))+x426+x425);
evalcond[4]=(((cj11*x426))+((cj11*x425))+(((-1.0)*new_r22*x421)));
evalcond[5]=((((-1.0)*new_r10*x424))+(((-1.0)*new_r20*x427))+(((-1.0)*new_r00*x422)));
evalcond[6]=((((-1.0)*new_r21*x427))+(((-1.0)*new_r01*x422))+(((-1.0)*new_r11*x424)));
evalcond[7]=((1.0)+(((-1.0)*new_r22*x427))+(((-1.0)*x421*x425))+(((-1.0)*x421*x426)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
IkReal j12eval[2];
IkReal x428=((1.0)*sj9);
IkReal x429=(cj9*sj6);
IkReal x430=x160;
IkReal x431=(cj4*sj8);
IkReal x432=(sj6*x428);
IkReal x433=(cj6*cj9);
IkReal x434=((((-1.0)*x432))+((cj8*x433)));
IkReal x435=(((cj9*x431))+((sj4*x434)));
IkReal x436=((1.0)*sj4*sj8);
IkReal x437=((((-1.0)*cj9*x436))+((cj4*x434)));
IkReal x438=(sj6*sj8);
IkReal x439=x169;
IkReal x440=x170;
IkReal x441=((((-1.0)*cj8*x432))+x433);
IkReal x442=x172;
IkReal x443=(((sj9*x431))+((sj4*x442)));
IkReal x444=(((cj4*x442))+(((-1.0)*sj4*sj8*x428)));
new_r00=(((r20*x430))+((r10*x435))+((r00*x437)));
new_r01=(((r11*x435))+((r01*x437))+((r21*x430)));
new_r02=(((r12*x435))+((r02*x437))+((r22*x430)));
new_r10=(((r20*x438))+((r00*x440))+((r10*x439)));
new_r11=(((r11*x439))+((r21*x438))+((r01*x440)));
new_r12=(((r22*x438))+((r12*x439))+((r02*x440)));
new_r20=(((r10*x443))+((r20*x441))+((r00*x444)));
new_r21=(((r01*x444))+((r21*x441))+((r11*x443)));
new_r22=(((r22*x441))+((r12*x443))+((r02*x444)));
j12eval[0]=sj11;
j12eval[1]=IKsign(sj11);
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  )
{
{
IkReal j12eval[2];
IkReal x445=((1.0)*sj9);
IkReal x446=(cj9*sj6);
IkReal x447=x160;
IkReal x448=(cj4*sj8);
IkReal x449=(sj6*x445);
IkReal x450=(cj6*cj9);
IkReal x451=(((cj8*x450))+(((-1.0)*x449)));
IkReal x452=(((cj9*x448))+((sj4*x451)));
IkReal x453=((1.0)*sj4*sj8);
IkReal x454=(((cj4*x451))+(((-1.0)*cj9*x453)));
IkReal x455=(sj6*sj8);
IkReal x456=x169;
IkReal x457=x170;
IkReal x458=(x450+(((-1.0)*cj8*x449)));
IkReal x459=x172;
IkReal x460=(((sj4*x459))+((sj9*x448)));
IkReal x461=(((cj4*x459))+(((-1.0)*sj4*sj8*x445)));
new_r00=(((r00*x454))+((r20*x447))+((r10*x452)));
new_r01=(((r01*x454))+((r21*x447))+((r11*x452)));
new_r02=(((r02*x454))+((r12*x452))+((r22*x447)));
new_r10=(((r20*x455))+((r00*x457))+((r10*x456)));
new_r11=(((r21*x455))+((r11*x456))+((r01*x457)));
new_r12=(((r12*x456))+((r22*x455))+((r02*x457)));
new_r20=(((r10*x460))+((r20*x458))+((r00*x461)));
new_r21=(((r11*x460))+((r21*x458))+((r01*x461)));
new_r22=(((r12*x460))+((r22*x458))+((r02*x461)));
j12eval[0]=sj10;
j12eval[1]=sj11;
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  )
{
{
IkReal j12eval[3];
IkReal x462=((1.0)*sj9);
IkReal x463=(cj9*sj6);
IkReal x464=x160;
IkReal x465=(cj4*sj8);
IkReal x466=(sj6*x462);
IkReal x467=(cj6*cj9);
IkReal x468=((((-1.0)*x466))+((cj8*x467)));
IkReal x469=(((cj9*x465))+((sj4*x468)));
IkReal x470=((1.0)*sj4*sj8);
IkReal x471=((((-1.0)*cj9*x470))+((cj4*x468)));
IkReal x472=(sj6*sj8);
IkReal x473=x169;
IkReal x474=x170;
IkReal x475=((((-1.0)*cj8*x466))+x467);
IkReal x476=x172;
IkReal x477=(((sj9*x465))+((sj4*x476)));
IkReal x478=((((-1.0)*sj4*sj8*x462))+((cj4*x476)));
new_r00=(((r10*x469))+((r00*x471))+((r20*x464)));
new_r01=(((r11*x469))+((r01*x471))+((r21*x464)));
new_r02=(((r22*x464))+((r02*x471))+((r12*x469)));
new_r10=(((r00*x474))+((r10*x473))+((r20*x472)));
new_r11=(((r01*x474))+((r11*x473))+((r21*x472)));
new_r12=(((r02*x474))+((r12*x473))+((r22*x472)));
new_r20=(((r20*x475))+((r10*x477))+((r00*x478)));
new_r21=(((r01*x478))+((r21*x475))+((r11*x477)));
new_r22=(((r22*x475))+((r02*x478))+((r12*x477)));
j12eval[0]=cj10;
j12eval[1]=cj11;
j12eval[2]=sj11;
if( IKabs(j12eval[0]) < 0.0000010000000000  || IKabs(j12eval[1]) < 0.0000010000000000  || IKabs(j12eval[2]) < 0.0000010000000000  )
{
{
IkReal evalcond[12];
bool bgotonextstatement = true;
do
{
IkReal x479=((1.0)*cj11);
IkReal x480=((((-1.0)*x479))+new_r22);
IkReal x481=((1.0)*sj11);
IkReal x482=((((-1.0)*x481))+new_r12);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j10)))), 6.28318530717959)));
evalcond[1]=x480;
evalcond[2]=x480;
evalcond[3]=new_r02;
evalcond[4]=x482;
evalcond[5]=x482;
evalcond[6]=((((-1.0)*new_r22*x481))+((cj11*new_r12)));
evalcond[7]=((((-1.0)*new_r10*x481))+(((-1.0)*new_r20*x479)));
evalcond[8]=((((-1.0)*new_r11*x481))+(((-1.0)*new_r21*x479)));
evalcond[9]=((1.0)+(((-1.0)*new_r12*x481))+(((-1.0)*new_r22*x479)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x483=IKPowWithIntegerCheck(IKsign(new_r12),-1);
if(!x483.valid){
continue;
}
CheckValue<IkReal> x484 = IKatan2WithCheck(IkReal(new_r21),((-1.0)*(((1.0)*new_r20))),IKFAST_ATAN2_MAGTHRESH);
if(!x484.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(((1.5707963267949)*(x483.value)))+(x484.value));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x485=IKcos(j12);
IkReal x486=IKsin(j12);
IkReal x487=((1.0)*new_r12);
IkReal x488=((1.0)*x485);
IkReal x489=((-1.0)*x488);
evalcond[0]=(((new_r12*x485))+new_r20);
evalcond[1]=(((new_r22*x486))+new_r11);
evalcond[2]=((((-1.0)*x486*x487))+new_r21);
evalcond[3]=((((-1.0)*new_r22*x488))+new_r10);
evalcond[4]=((((-1.0)*(1.0)*new_r00))+(((-1.0)*x486)));
evalcond[5]=((((-1.0)*(1.0)*new_r01))+x489);
evalcond[6]=(((new_r11*new_r22))+(((-1.0)*new_r21*x487))+x486);
evalcond[7]=((((-1.0)*new_r20*x487))+((new_r10*new_r22))+x489);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x490=((1.0)*cj11);
IkReal x491=((((-1.0)*x490))+new_r22);
IkReal x492=((1.0)*sj11);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j10)))), 6.28318530717959)));
evalcond[1]=x491;
evalcond[2]=x491;
evalcond[3]=new_r02;
evalcond[4]=(sj11+new_r12);
evalcond[5]=((((-1.0)*(1.0)*new_r12))+(((-1.0)*x492)));
evalcond[6]=((((-1.0)*new_r22*x492))+(((-1.0)*new_r12*x490)));
evalcond[7]=(((new_r10*sj11))+(((-1.0)*new_r20*x490)));
evalcond[8]=((((-1.0)*new_r21*x490))+((new_r11*sj11)));
evalcond[9]=((1.0)+(((-1.0)*new_r22*x490))+((new_r12*sj11)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r00) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r01) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r00)+IKsqr(new_r01)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r00, new_r01);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x493=IKsin(j12);
IkReal x494=((1.0)*(IKcos(j12)));
IkReal x495=((-1.0)*x494);
IkReal x496=((1.0)*new_r11);
IkReal x497=((1.0)*new_r10);
evalcond[0]=(((new_r12*x493))+new_r21);
evalcond[1]=(new_r00+(((-1.0)*x493)));
evalcond[2]=(new_r01+x495);
evalcond[3]=((((-1.0)*new_r12*x494))+new_r20);
evalcond[4]=(((new_r22*x493))+(((-1.0)*x496)));
evalcond[5]=((((-1.0)*new_r22*x494))+(((-1.0)*x497)));
evalcond[6]=(((new_r12*new_r21))+(((-1.0)*new_r22*x496))+x493);
evalcond[7]=(((new_r12*new_r20))+(((-1.0)*new_r22*x497))+x495);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x498=((1.0)*cj10);
IkReal x499=((1.0)*sj10);
IkReal x500=((((-1.0)*new_r02*x499))+((cj10*new_r12)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-1.5707963267949)+j11)))), 6.28318530717959)));
evalcond[1]=new_r22;
evalcond[2]=((((-1.0)*x498))+new_r02);
evalcond[3]=((((-1.0)*x499))+new_r12);
evalcond[4]=x500;
evalcond[5]=x500;
evalcond[6]=((-1.0)+((cj10*new_r02))+((new_r12*sj10)));
evalcond[7]=(((cj10*new_r01))+((new_r11*sj10)));
evalcond[8]=(((cj10*new_r00))+((new_r10*sj10)));
evalcond[9]=((((-1.0)*new_r10*x499))+(((-1.0)*new_r00*x498)));
evalcond[10]=((((-1.0)*new_r11*x499))+(((-1.0)*new_r01*x498)));
evalcond[11]=((1.0)+(((-1.0)*new_r02*x498))+(((-1.0)*new_r12*x499)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  && IKabs(evalcond[10]) < 0.0000010000000000  && IKabs(evalcond[11]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r21) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*(((1.0)*new_r20)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r21)+IKsqr(((-1.0)*(((1.0)*new_r20))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r21, ((-1.0)*(((1.0)*new_r20))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x501=IKcos(j12);
IkReal x502=IKsin(j12);
IkReal x503=((1.0)*x502);
IkReal x504=((-1.0)*x503);
IkReal x505=((1.0)*x501);
IkReal x506=((1.0)*new_r12);
evalcond[0]=(x501+new_r20);
evalcond[1]=(x504+new_r21);
evalcond[2]=(((new_r12*x501))+new_r01);
evalcond[3]=(((new_r12*x502))+new_r00);
evalcond[4]=((((-1.0)*new_r02*x505))+new_r11);
evalcond[5]=((((-1.0)*new_r02*x503))+new_r10);
evalcond[6]=((((-1.0)*new_r00*x506))+x504+((new_r02*new_r10)));
evalcond[7]=((((-1.0)*new_r01*x506))+((new_r02*new_r11))+(((-1.0)*x505)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x507=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x508=((1.0)+((cj10*new_r02))+((new_r12*sj10)));
IkReal x509=(((cj10*new_r01))+((new_r11*sj10)));
IkReal x510=(((cj10*new_r00))+((new_r10*sj10)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((1.5707963267949)+j11)))), 6.28318530717959)));
evalcond[1]=new_r22;
evalcond[2]=(cj10+new_r02);
evalcond[3]=(sj10+new_r12);
evalcond[4]=x507;
evalcond[5]=x507;
evalcond[6]=x508;
evalcond[7]=x509;
evalcond[8]=x510;
evalcond[9]=x510;
evalcond[10]=x509;
evalcond[11]=x508;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  && IKabs(evalcond[10]) < 0.0000010000000000  && IKabs(evalcond[11]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(((-1.0)*(((1.0)*new_r21)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r20) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((-1.0)*(((1.0)*new_r21))))+IKsqr(new_r20)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((-1.0)*(((1.0)*new_r21))), new_r20);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x511=IKsin(j12);
IkReal x512=IKcos(j12);
IkReal x513=((1.0)*x512);
IkReal x514=((-1.0)*x513);
IkReal x515=((1.0)*x511);
IkReal x516=((1.0)*new_r02);
evalcond[0]=(x511+new_r21);
evalcond[1]=(x514+new_r20);
evalcond[2]=(((new_r02*x512))+new_r11);
evalcond[3]=(((new_r02*x511))+new_r10);
evalcond[4]=((((-1.0)*new_r12*x513))+new_r01);
evalcond[5]=((((-1.0)*new_r12*x515))+new_r00);
evalcond[6]=((((-1.0)*new_r10*x516))+((new_r00*new_r12))+(((-1.0)*x515)));
evalcond[7]=((((-1.0)*new_r11*x516))+((new_r01*new_r12))+x514);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x517=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x518=(((cj10*new_r02))+((new_r12*sj10)));
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j11))), 6.28318530717959)));
evalcond[1]=((-1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
evalcond[6]=x517;
evalcond[7]=x517;
evalcond[8]=x518;
evalcond[9]=x518;
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x519=((1.0)*new_r01);
if( IKabs(((((-1.0)*cj10*x519))+(((-1.0)*(1.0)*new_r00*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs((((cj10*new_r00))+(((-1.0)*sj10*x519)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((-1.0)*cj10*x519))+(((-1.0)*(1.0)*new_r00*sj10))))+IKsqr((((cj10*new_r00))+(((-1.0)*sj10*x519))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((((-1.0)*cj10*x519))+(((-1.0)*(1.0)*new_r00*sj10))), (((cj10*new_r00))+(((-1.0)*sj10*x519))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x520=IKsin(j12);
IkReal x521=(cj10*x520);
IkReal x522=IKcos(j12);
IkReal x523=((1.0)*x522);
IkReal x524=((-1.0)*x523);
IkReal x525=((1.0)*sj10);
IkReal x526=((((-1.0)*cj10*x523))+((sj10*x520)));
evalcond[0]=(((cj10*new_r01))+((new_r11*sj10))+x520);
evalcond[1]=(((sj10*x522))+new_r01+x521);
evalcond[2]=(((cj10*new_r00))+((new_r10*sj10))+x524);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*new_r00*x525))+(((-1.0)*x520)));
evalcond[4]=(((cj10*new_r11))+(((-1.0)*new_r01*x525))+x524);
evalcond[5]=(new_r00+x526);
evalcond[6]=(new_r11+x526);
evalcond[7]=((((-1.0)*x521))+(((-1.0)*x522*x525))+new_r10);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x527=((((-1.0)*(1.0)*new_r02*sj10))+((cj10*new_r12)));
IkReal x528=(cj10*new_r02);
IkReal x529=(new_r12*sj10);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j11)))), 6.28318530717959)));
evalcond[1]=((1.0)+new_r22);
evalcond[2]=new_r20;
evalcond[3]=new_r02;
evalcond[4]=new_r12;
evalcond[5]=new_r21;
evalcond[6]=x527;
evalcond[7]=x527;
evalcond[8]=(x529+x528);
evalcond[9]=((((-1.0)*x528))+(((-1.0)*x529)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
IkReal x530=((1.0)*new_r00);
if( IKabs(((((-1.0)*sj10*x530))+((cj10*new_r01)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((((-1.0)*cj10*x530))+(((-1.0)*(1.0)*new_r01*sj10)))) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(((((-1.0)*sj10*x530))+((cj10*new_r01))))+IKsqr(((((-1.0)*cj10*x530))+(((-1.0)*(1.0)*new_r01*sj10))))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(((((-1.0)*sj10*x530))+((cj10*new_r01))), ((((-1.0)*cj10*x530))+(((-1.0)*(1.0)*new_r01*sj10))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x531=IKcos(j12);
IkReal x532=IKsin(j12);
IkReal x533=((1.0)*x532);
IkReal x534=((-1.0)*x533);
IkReal x535=(cj10*x531);
IkReal x536=((1.0)*sj10);
IkReal x537=((((-1.0)*cj10*x533))+((sj10*x531)));
evalcond[0]=(x531+((cj10*new_r00))+((new_r10*sj10)));
evalcond[1]=(x534+((cj10*new_r01))+((new_r11*sj10)));
evalcond[2]=(x535+((sj10*x532))+new_r00);
evalcond[3]=(x534+((cj10*new_r10))+(((-1.0)*new_r00*x536)));
evalcond[4]=(((cj10*new_r11))+(((-1.0)*x531))+(((-1.0)*new_r01*x536)));
evalcond[5]=(x537+new_r01);
evalcond[6]=(x537+new_r10);
evalcond[7]=((((-1.0)*x532*x536))+new_r11+(((-1.0)*x535)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x538=((1.0)*cj11);
IkReal x539=((((-1.0)*x538))+new_r22);
IkReal x540=((1.0)*sj11);
IkReal x541=((((-1.0)*x540))+new_r02);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(j10))), 6.28318530717959)));
evalcond[1]=x539;
evalcond[2]=x539;
evalcond[3]=x541;
evalcond[4]=new_r12;
evalcond[5]=x541;
evalcond[6]=((((-1.0)*new_r22*x540))+((cj11*new_r02)));
evalcond[7]=((((-1.0)*new_r20*x538))+(((-1.0)*new_r00*x540)));
evalcond[8]=((((-1.0)*new_r21*x538))+(((-1.0)*new_r01*x540)));
evalcond[9]=((1.0)+(((-1.0)*new_r02*x540))+(((-1.0)*new_r22*x538)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
if( IKabs(new_r10) < IKFAST_ATAN2_MAGTHRESH && IKabs(new_r11) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr(new_r10)+IKsqr(new_r11)-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2(new_r10, new_r11);
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x542=IKcos(j12);
IkReal x543=IKsin(j12);
IkReal x544=((1.0)*x542);
IkReal x545=((-1.0)*x544);
IkReal x546=((1.0)*new_r02);
evalcond[0]=(((new_r02*x542))+new_r20);
evalcond[1]=((((-1.0)*x543))+new_r10);
evalcond[2]=(new_r11+x545);
evalcond[3]=(((new_r22*x543))+new_r01);
evalcond[4]=((((-1.0)*x543*x546))+new_r21);
evalcond[5]=((((-1.0)*new_r22*x544))+new_r00);
evalcond[6]=(((new_r01*new_r22))+(((-1.0)*new_r21*x546))+x543);
evalcond[7]=((((-1.0)*new_r20*x546))+((new_r00*new_r22))+x545);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
IkReal x547=((1.0)*cj11);
IkReal x548=((((-1.0)*x547))+new_r22);
IkReal x549=((1.0)*sj11);
evalcond[0]=((-3.14159265358979)+(IKfmod(((3.14159265358979)+(IKabs(((-3.14159265358979)+j10)))), 6.28318530717959)));
evalcond[1]=x548;
evalcond[2]=x548;
evalcond[3]=(sj11+new_r02);
evalcond[4]=new_r12;
evalcond[5]=((((-1.0)*x549))+(((-1.0)*(1.0)*new_r02)));
evalcond[6]=((((-1.0)*new_r22*x549))+(((-1.0)*new_r02*x547)));
evalcond[7]=((((-1.0)*new_r20*x547))+((new_r00*sj11)));
evalcond[8]=((((-1.0)*new_r21*x547))+((new_r01*sj11)));
evalcond[9]=((1.0)+((new_r02*sj11))+(((-1.0)*new_r22*x547)));
if( IKabs(evalcond[0]) < 0.0000010000000000  && IKabs(evalcond[1]) < 0.0000010000000000  && IKabs(evalcond[2]) < 0.0000010000000000  && IKabs(evalcond[3]) < 0.0000010000000000  && IKabs(evalcond[4]) < 0.0000010000000000  && IKabs(evalcond[5]) < 0.0000010000000000  && IKabs(evalcond[6]) < 0.0000010000000000  && IKabs(evalcond[7]) < 0.0000010000000000  && IKabs(evalcond[8]) < 0.0000010000000000  && IKabs(evalcond[9]) < 0.0000010000000000  )
{
bgotonextstatement=false;
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x550 = IKatan2WithCheck(IkReal(((-1.0)*(((1.0)*new_r21)))),new_r20,IKFAST_ATAN2_MAGTHRESH);
if(!x550.valid){
continue;
}
CheckValue<IkReal> x551=IKPowWithIntegerCheck(IKsign(new_r02),-1);
if(!x551.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(x550.value)+(((1.5707963267949)*(x551.value))));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[8];
IkReal x552=IKsin(j12);
IkReal x553=((1.0)*(IKcos(j12)));
IkReal x554=((-1.0)*x553);
IkReal x555=((1.0)*new_r01);
IkReal x556=((1.0)*new_r00);
evalcond[0]=(((new_r02*x552))+new_r21);
evalcond[1]=((((-1.0)*new_r02*x553))+new_r20);
evalcond[2]=((((-1.0)*(1.0)*new_r10))+(((-1.0)*x552)));
evalcond[3]=(x554+(((-1.0)*(1.0)*new_r11)));
evalcond[4]=(((new_r22*x552))+(((-1.0)*x555)));
evalcond[5]=((((-1.0)*x556))+(((-1.0)*new_r22*x553)));
evalcond[6]=(((new_r02*new_r21))+(((-1.0)*new_r22*x555))+x552);
evalcond[7]=(((new_r02*new_r20))+x554+(((-1.0)*new_r22*x556)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}
} while(0);
if( bgotonextstatement )
{
bool bgotonextstatement = true;
do
{
if( 1 )
{
bgotonextstatement=false;
continue; // branch miss [j12]

}
} while(0);
if( bgotonextstatement )
{
}
}
}
}
}
}
}
}
}
}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x558=IKPowWithIntegerCheck(sj11,-1);
if(!x558.valid){
continue;
}
IkReal x557=x558.value;
CheckValue<IkReal> x559=IKPowWithIntegerCheck(cj10,-1);
if(!x559.valid){
continue;
}
CheckValue<IkReal> x560=IKPowWithIntegerCheck(cj11,-1);
if(!x560.valid){
continue;
}
if( IKabs((x557*(x559.value)*(x560.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*new_r20*x557)) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((x557*(x559.value)*(x560.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10))))))+IKsqr(((-1.0)*new_r20*x557))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2((x557*(x559.value)*(x560.value)*(((((-1.0)*(1.0)*new_r01*sj11))+((new_r20*sj10))))), ((-1.0)*new_r20*x557));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x561=IKcos(j12);
IkReal x562=IKsin(j12);
IkReal x563=((1.0)*sj11);
IkReal x564=(cj10*new_r01);
IkReal x565=(new_r11*sj10);
IkReal x566=(cj11*x562);
IkReal x567=((1.0)*sj10);
IkReal x568=((1.0)*x562);
IkReal x569=((1.0)*x561);
IkReal x570=((-1.0)*x569);
IkReal x571=(cj10*new_r00);
IkReal x572=(new_r10*sj10);
IkReal x573=(cj10*x569);
evalcond[0]=(((sj11*x561))+new_r20);
evalcond[1]=((((-1.0)*x562*x563))+new_r21);
evalcond[2]=(x564+x565+x566);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x568))+(((-1.0)*new_r00*x567)));
evalcond[4]=(((cj10*new_r11))+x570+(((-1.0)*new_r01*x567)));
evalcond[5]=(((cj10*x566))+new_r01+((sj10*x561)));
evalcond[6]=(x572+x571+(((-1.0)*cj11*x569)));
evalcond[7]=(((sj10*x562))+(((-1.0)*cj11*x573))+new_r00);
evalcond[8]=((((-1.0)*x573))+new_r11+((sj10*x566)));
evalcond[9]=((((-1.0)*cj10*x568))+(((-1.0)*cj11*x561*x567))+new_r10);
evalcond[10]=((((-1.0)*new_r21*x563))+((cj11*x565))+((cj11*x564))+x562);
evalcond[11]=(x570+((cj11*x572))+((cj11*x571))+(((-1.0)*new_r20*x563)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x576=IKPowWithIntegerCheck(sj11,-1);
if(!x576.valid){
continue;
}
IkReal x574=x576.value;
IkReal x575=((1.0)*new_r20);
CheckValue<IkReal> x577=IKPowWithIntegerCheck(sj10,-1);
if(!x577.valid){
continue;
}
if( IKabs((x574*(x577.value)*(((((-1.0)*cj10*cj11*x575))+(((-1.0)*(1.0)*new_r00*sj11)))))) < IKFAST_ATAN2_MAGTHRESH && IKabs(((-1.0)*x574*x575)) < IKFAST_ATAN2_MAGTHRESH && IKabs(IKsqr((x574*(x577.value)*(((((-1.0)*cj10*cj11*x575))+(((-1.0)*(1.0)*new_r00*sj11))))))+IKsqr(((-1.0)*x574*x575))-1) <= IKFAST_SINCOS_THRESH )
    continue;
j12array[0]=IKatan2((x574*(x577.value)*(((((-1.0)*cj10*cj11*x575))+(((-1.0)*(1.0)*new_r00*sj11))))), ((-1.0)*x574*x575));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x578=IKcos(j12);
IkReal x579=IKsin(j12);
IkReal x580=((1.0)*sj11);
IkReal x581=(cj10*new_r01);
IkReal x582=(new_r11*sj10);
IkReal x583=(cj11*x579);
IkReal x584=((1.0)*sj10);
IkReal x585=((1.0)*x579);
IkReal x586=((1.0)*x578);
IkReal x587=((-1.0)*x586);
IkReal x588=(cj10*new_r00);
IkReal x589=(new_r10*sj10);
IkReal x590=(cj10*x586);
evalcond[0]=(((sj11*x578))+new_r20);
evalcond[1]=((((-1.0)*x579*x580))+new_r21);
evalcond[2]=(x581+x582+x583);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x585))+(((-1.0)*new_r00*x584)));
evalcond[4]=(((cj10*new_r11))+(((-1.0)*new_r01*x584))+x587);
evalcond[5]=(((sj10*x578))+((cj10*x583))+new_r01);
evalcond[6]=((((-1.0)*cj11*x586))+x589+x588);
evalcond[7]=(new_r00+((sj10*x579))+(((-1.0)*cj11*x590)));
evalcond[8]=((((-1.0)*x590))+((sj10*x583))+new_r11);
evalcond[9]=((((-1.0)*cj10*x585))+(((-1.0)*cj11*x578*x584))+new_r10);
evalcond[10]=(x579+((cj11*x581))+((cj11*x582))+(((-1.0)*new_r21*x580)));
evalcond[11]=(((cj11*x589))+(((-1.0)*new_r20*x580))+((cj11*x588))+x587);
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}

} else
{
{
IkReal j12array[1], cj12array[1], sj12array[1];
bool j12valid[1]={false};
_nj12 = 1;
CheckValue<IkReal> x591=IKPowWithIntegerCheck(IKsign(sj11),-1);
if(!x591.valid){
continue;
}
CheckValue<IkReal> x592 = IKatan2WithCheck(IkReal(new_r21),((-1.0)*(((1.0)*new_r20))),IKFAST_ATAN2_MAGTHRESH);
if(!x592.valid){
continue;
}
j12array[0]=((-1.5707963267949)+(((1.5707963267949)*(x591.value)))+(x592.value));
sj12array[0]=IKsin(j12array[0]);
cj12array[0]=IKcos(j12array[0]);
if( j12array[0] > IKPI )
{
    j12array[0]-=IK2PI;
}
else if( j12array[0] < -IKPI )
{    j12array[0]+=IK2PI;
}
j12valid[0] = true;
for(int ij12 = 0; ij12 < 1; ++ij12)
{
if( !j12valid[ij12] )
{
    continue;
}
_ij12[0] = ij12; _ij12[1] = -1;
for(int iij12 = ij12+1; iij12 < 1; ++iij12)
{
if( j12valid[iij12] && IKabs(cj12array[ij12]-cj12array[iij12]) < IKFAST_SOLUTION_THRESH && IKabs(sj12array[ij12]-sj12array[iij12]) < IKFAST_SOLUTION_THRESH )
{
    j12valid[iij12]=false; _ij12[1] = iij12; break;
}
}
j12 = j12array[ij12]; cj12 = cj12array[ij12]; sj12 = sj12array[ij12];
{
IkReal evalcond[12];
IkReal x593=IKcos(j12);
IkReal x594=IKsin(j12);
IkReal x595=((1.0)*sj11);
IkReal x596=(cj10*new_r01);
IkReal x597=(new_r11*sj10);
IkReal x598=(cj11*x594);
IkReal x599=((1.0)*sj10);
IkReal x600=((1.0)*x594);
IkReal x601=((1.0)*x593);
IkReal x602=((-1.0)*x601);
IkReal x603=(cj10*new_r00);
IkReal x604=(new_r10*sj10);
IkReal x605=(cj10*x601);
evalcond[0]=(new_r20+((sj11*x593)));
evalcond[1]=((((-1.0)*x594*x595))+new_r21);
evalcond[2]=(x598+x596+x597);
evalcond[3]=(((cj10*new_r10))+(((-1.0)*x600))+(((-1.0)*new_r00*x599)));
evalcond[4]=(((cj10*new_r11))+(((-1.0)*new_r01*x599))+x602);
evalcond[5]=(((cj10*x598))+((sj10*x593))+new_r01);
evalcond[6]=((((-1.0)*cj11*x601))+x603+x604);
evalcond[7]=((((-1.0)*cj11*x605))+((sj10*x594))+new_r00);
evalcond[8]=(((sj10*x598))+new_r11+(((-1.0)*x605)));
evalcond[9]=((((-1.0)*cj10*x600))+(((-1.0)*cj11*x593*x599))+new_r10);
evalcond[10]=(((cj11*x597))+x594+(((-1.0)*new_r21*x595))+((cj11*x596)));
evalcond[11]=(((cj11*x603))+x602+((cj11*x604))+(((-1.0)*new_r20*x595)));
if( IKabs(evalcond[0]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[1]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[2]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[3]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[4]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[5]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[6]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[7]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[8]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[9]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[10]) > IKFAST_EVALCOND_THRESH  || IKabs(evalcond[11]) > IKFAST_EVALCOND_THRESH  )
{
continue;
}
}

{
std::vector<IkSingleDOFSolutionBase<IkReal> > vinfos(7);
vinfos[0].jointtype = 1;
vinfos[0].foffset = j4;
vinfos[0].indices[0] = _ij4[0];
vinfos[0].indices[1] = _ij4[1];
vinfos[0].maxsolutions = _nj4;
vinfos[1].jointtype = 1;
vinfos[1].foffset = j6;
vinfos[1].indices[0] = _ij6[0];
vinfos[1].indices[1] = _ij6[1];
vinfos[1].maxsolutions = _nj6;
vinfos[2].jointtype = 1;
vinfos[2].foffset = j8;
vinfos[2].indices[0] = _ij8[0];
vinfos[2].indices[1] = _ij8[1];
vinfos[2].maxsolutions = _nj8;
vinfos[3].jointtype = 1;
vinfos[3].foffset = j9;
vinfos[3].indices[0] = _ij9[0];
vinfos[3].indices[1] = _ij9[1];
vinfos[3].maxsolutions = _nj9;
vinfos[4].jointtype = 1;
vinfos[4].foffset = j10;
vinfos[4].indices[0] = _ij10[0];
vinfos[4].indices[1] = _ij10[1];
vinfos[4].maxsolutions = _nj10;
vinfos[5].jointtype = 1;
vinfos[5].foffset = j11;
vinfos[5].indices[0] = _ij11[0];
vinfos[5].indices[1] = _ij11[1];
vinfos[5].maxsolutions = _nj11;
vinfos[6].jointtype = 1;
vinfos[6].foffset = j12;
vinfos[6].indices[0] = _ij12[0];
vinfos[6].indices[1] = _ij12[1];
vinfos[6].maxsolutions = _nj12;
std::vector<int> vfree(0);
solutions.AddSolution(vinfos,vfree);
}
}
}

}

}
}
}

}

}
}
}
}
}};


/// solves the inverse kinematics equations.
/// \param pfree is an array specifying the free joints of the chain.
IKFAST_API bool ComputeIk(const IkReal* eetrans, const IkReal* eerot, const IkReal* pfree, IkSolutionListBase<IkReal>& solutions) {
IKSolver solver;
return solver.ComputeIk(eetrans,eerot,pfree,solutions);
}

IKFAST_API bool ComputeIk2(const IkReal* eetrans, const IkReal* eerot, const IkReal* pfree, IkSolutionListBase<IkReal>& solutions, void* pOpenRAVEManip) {
IKSolver solver;
return solver.ComputeIk(eetrans,eerot,pfree,solutions);
}

IKFAST_API const char* GetKinematicsHash() { return "268c2c509bc1bb657da055f0ef2eb7e1"; }

IKFAST_API const char* GetIkFastVersion() { return IKFAST_STRINGIZE(IKFAST_VERSION); }

#ifdef IKFAST_NAMESPACE
} // end namespace
#endif

//==============================================================================
SharedLibraryWamIkFast::SharedLibraryWamIkFast(
    dart::dynamics::InverseKinematics* ik,
    const std::vector<std::size_t>& dofMap,
    const std::vector<std::size_t>& freeDofMap,
    const std::string& methodName,
    const dart::dynamics::InverseKinematics::Analytical::Properties& properties)
  : IkFast{ik, dofMap, freeDofMap, methodName, properties}
{
  // Do nothing
}

//==============================================================================
std::unique_ptr<dart::dynamics::InverseKinematics::GradientMethod>
SharedLibraryWamIkFast::clone(dart::dynamics::InverseKinematics* newIK) const
{
  return dart::common::make_unique<SharedLibraryWamIkFast>(
      newIK, mDofs, mFreeDofs, getMethodName(), getAnalyticalProperties());
}

//==============================================================================
int SharedLibraryWamIkFast::getNumFreeParameters() const
{
  return GetNumFreeParameters();
}

//==============================================================================
int* SharedLibraryWamIkFast::getFreeParameters() const
{
  return GetFreeParameters();
}

//==============================================================================
int SharedLibraryWamIkFast::getNumJoints() const
{
  return GetNumJoints();
}

//==============================================================================
int SharedLibraryWamIkFast::getIkRealSize() const
{
  return GetIkRealSize();
}

//==============================================================================
int SharedLibraryWamIkFast::getIkType() const
{
  return GetIkType();
}

//==============================================================================
bool SharedLibraryWamIkFast::computeIk(
    const IkReal* targetTranspose,
    const IkReal* targetRotation,
    const IkReal* freeParams,
    ikfast::IkSolutionListBase<IkReal>& solutions)
{
  return ComputeIk(targetTranspose, targetRotation, freeParams, solutions);
}

//==============================================================================
const char* SharedLibraryWamIkFast::getKinematicsHash()
{
  return GetKinematicsHash();
}

//==============================================================================
const char* SharedLibraryWamIkFast::getIkFastVersion()
{
  return GetIkFastVersion();
}
