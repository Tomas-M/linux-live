#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// Small streaming SHA-256 implementation; no platform crypto DLL or runtime package is needed.
class Hash
{
   uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
   unsigned char buffer[64]={};
   uint64_t bytes=0;
   static uint32_t rotate(uint32_t value,int bits) { return value>>bits|value<<(32-bits); }

   // Compress one complete FIPS 180-4 message block.
   void block(const unsigned char *data)
   {
      static const uint32_t k[]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
      uint32_t w[64];
      for (int i=0; i<16; i++) w[i]=uint32_t(data[i*4])<<24|uint32_t(data[i*4+1])<<16|uint32_t(data[i*4+2])<<8|data[i*4+3];
      for (int i=16; i<64; i++) w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
      uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
      for (int i=0; i<64; i++)
      {
         uint32_t first=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
         uint32_t second=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
         h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
      }
      state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
   }
public:
   // Feed arbitrary-sized file chunks without buffering the payload in memory.
   void add(const void *value,size_t size)
   {
      auto data=static_cast<const unsigned char *>(value);
      while (size)
      {
         size_t at=bytes%64,count=64-at; if (count>size) count=size;
         memcpy(buffer+at,data,count); bytes+=count; data+=count; size-=count;
         if (bytes%64==0) block(buffer);
      }
   }

   // Finalize a copy so callers can retain or compare the current stream state.
   std::array<unsigned char,32> finish() const
   {
      Hash copy=*this; uint64_t bits=bytes*8; unsigned char end[128]={0x80};
      size_t padding=bytes%64<56?56-bytes%64:120-bytes%64;
      for (int i=0; i<8; i++) end[padding+i]=bits>>(56-i*8);
      copy.add(end,padding+8); std::array<unsigned char,32> result;
      for (int i=0; i<32; i++) result[i]=copy.state[i/4]>>(24-(i%4)*8);
      return result;
   }
};
