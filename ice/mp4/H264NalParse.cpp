#include <string.h>
#include "H264NalParse.h"

void init_put_bits(PutBitContext *s, char *buffer, int buffer_size)
{
    if(buffer_size < 0) {
        buffer_size = 0;
        buffer = 0;
    }

    s->size_in_bits= 8*buffer_size;
    s->buf = buffer;
    s->buf_end = s->buf + buffer_size;
    s->buf_ptr = s->buf;
    s->bit_left=32;
    s->bit_buf=0;
}

void flush_put_bits(PutBitContext *s)
{
    s->bit_buf<<= s->bit_left;
    while (s->bit_left < 32) {
        *s->buf_ptr++=s->bit_buf >> 24;
        s->bit_buf<<=8;
        s->bit_left+=8;
    }
    s->bit_left=32;
    s->bit_buf=0;
}


void put_bits(PutBitContext *s, int n, unsigned int value)
{
    unsigned int bit_buf;
    int bit_left;

    bit_buf = s->bit_buf;
    bit_left = s->bit_left;

    if (n < bit_left) {
        bit_buf = (bit_buf<<n) | value;
        bit_left-=n;
    } else {
        bit_buf<<=bit_left;
        bit_buf |= value >> (n - bit_left);
        UI32ToBytes((uint8_t*)s->buf_ptr, bit_buf);
        //AV_WB32(s->buf_ptr, bit_buf);
        //printf("bitbuf = %08x\n", bit_buf);
        s->buf_ptr+=4;
        bit_left+=32 - n;
        bit_buf = value;
    }

    s->bit_buf = bit_buf;
    s->bit_left = bit_left;
}

uint8_t* UI08ToBytes(uint8_t* buf, unsigned char val)
{
   buf[0] = (char)(val) & 0xff;
   return buf + 1;
}

uint8_t* UI16ToBytes(uint8_t* buf, unsigned short val)
{
   buf[0] = (char)(val >> 8) & 0xff;
   buf[1] = (char)(val) & 0xff;
   return buf + 2;
}

uint8_t* UI24ToBytes(uint8_t* buf, unsigned int val)
{
   buf[0] = (char)(val >> 16) & 0xff;
   buf[1] = (char)(val >> 8) & 0xff;
   buf[2] = (char)(val) & 0xff;
   return buf + 3;
}

uint8_t* UI32ToBytes(uint8_t* buf, unsigned int val)
{
   buf[0] = (char)(val >> 24) & 0xff;
   buf[1] = (char)(val >> 16) & 0xff;
   buf[2] = (char)(val >> 8) & 0xff;
   buf[3] = (char)(val) & 0xff;
   return buf + 4;
}


uint8_t* UI64ToBytes(uint8_t* buf, uint64_t val)
{
   buf[0] = (char)(val >> 56) & 0xff;
   buf[1] = (char)(val >> 48) & 0xff;
   buf[2] = (char)(val >> 40) & 0xff;
   buf[3] = (char)(val >> 32) & 0xff;
   buf[4] = (char)(val >> 24) & 0xff;
   buf[5] = (char)(val >> 16) & 0xff;
   buf[6] = (char)(val >> 8) & 0xff;
   buf[7] = (char)(val) & 0xff;
   return buf + 8;
}

uint8_t* DoubleToBytes(uint8_t* buf, double val) 
{

   union {
       unsigned char dc[8];
       double dd;
   } d;

   d.dd = val;
   for(int i=0;i<8;i++){
	   *buf++ = d.dc[7-i];
   }
   return buf;
   /*
   unsigned char b[8];
   b[0] = d.dc[7];
   b[1] = d.dc[6];
   b[2] = d.dc[5];
   b[3] = d.dc[4];
   b[4] = d.dc[3];
   b[5] = d.dc[2];
   b[6] = d.dc[1];
   b[7] = d.dc[0];

   memcpy(buf, b, 8);
   return buf + 8;
	*/
}

uint32_t BytesToUI32(const uint8_t* buf)
{
   return ( (((unsigned int)buf[0]) << 24)& 0xff000000 )
       | ( (((unsigned int)buf[1]) << 16)& 0xff0000 )
       | ( (((unsigned int)buf[2]) << 8)& 0xff00 )
       | ( (((unsigned int)buf[3]))& 0xff );
}

uint32_t BytesToUI24(const uint8_t* buf)
{
	return ( (((unsigned int)buf[0]) << 16)& 0xff0000 )
		| ( (((unsigned int)buf[1]) << 8)& 0xff00 )
		| ( (((unsigned int)buf[2]))& 0xff );
}

uint16_t BytesToUI16(const uint8_t* buf){
	return ((((uint16_t)buf[0]) << 8) & 0xff00)
    | ((((uint16_t)buf[1])) & 0xff);
}

const char* AVCFindStartCodeInternal(const char *p, const char *end)
{
#ifdef _WIN32
   const char *a = p + 4 - ((intptr_t)p & 3);
#else
  const char *a = p;
#endif
   for (end -= 3; p < a && p < end; p++) { // 000001 NAL头部
       if (p[0] == 0 && p[1] == 0 && p[2] == 1)
           return p;
   }


   for (end -= 3; p < end; p += 4) {
       unsigned int x = *(const unsigned int*)p;
       //      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
       //      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
       if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
           if (p[1] == 0) {
               if (p[0] == 0 && p[2] == 1)
                   return p;
               if (p[2] == 0 && p[3] == 1)
                   return p+1;
           }

           if (p[3] == 0) {
               if (p[2] == 0 && p[4] == 1)
                   return p+2;
               if (p[4] == 0 && p[5] == 1)
                   return p+3;
           }
       }
   }

   for (end += 3; p < end; p++) {
       if (p[0] == 0 && p[1] == 0 && p[2] == 1)
           return p;
   }

   return end + 3;
}


const char* AVCFindStartCode(const char *p, const char *end)
{
   const char *out= AVCFindStartCodeInternal(p, end);
   if(p<out && out<end && !out[-1]) out--;
   return out;
}

bool AVCParseNalUnits(const char *bufIn, int inSize, uint8_t* bufOut, int* outSize)
{
   const char *p = bufIn;
   const char *end = p + inSize;
   const char *nal_start, *nal_end;

   uint8_t* pbuf = bufOut;
   *outSize = 0;

// 查找NAL起始字节
   nal_start = AVCFindStartCode(p, end);
   while (nal_start < end)
   {
       while(!*(nal_start++));

       nal_end = AVCFindStartCode(nal_start, end);
       unsigned int nal_size = nal_end - nal_start;
       // 把4字节的NAL头部写成长度
       pbuf = UI32ToBytes(pbuf, nal_size);
       memcpy(pbuf, nal_start, nal_size);
       pbuf += nal_size;

       nal_start = nal_end;
   }

   *outSize = (pbuf - bufOut);
   return true;
}

bool ParseNalFrame(const char* buf, int size, 
  char* outBuf, int& outLen,
  std::string* spsBuf, std::string* ppsBuf, std::string* vps,
  bool& isKeyframe, bool h264)
{

  if (!AVCParseNalUnits(buf, size, (uint8_t*)outBuf, &outLen))
    return false;
  return ParseAvcFrame(outBuf, outLen, false, spsBuf, ppsBuf, vps, isKeyframe, h264);
}

bool ParseAvcFrame(char* buf, int size, bool toNal,
					std::string* spsBuf, std::string* ppsBuf, std::string* vps,
					bool& isKeyframe, bool h264)
{
   char* start = buf;
   char* end = start + size;

   /* look for sps and pps */
   while (start < end) 
   {
       unsigned int size = BytesToUI32((uint8_t*)start);
       if (h264) {
         unsigned char nal_type = H264_TYPE(start[4]);
         switch(nal_type) {
         case AVC_NAL_SPS: /* SPS */
            if (spsBuf) spsBuf->assign(start + 4, size);
            break;
         case AVC_NAL_PPS: /* PPS */
            if (ppsBuf) ppsBuf->assign(start + 4, size);
            break;
         case AVC_NAL_IDR:
            isKeyframe = true;
            break;
         }
       } else {
         // h265 nal 判断
         unsigned char nal_type = H265_TYPE(start[4]);
         switch (nal_type) {
         case HEVC_NAL_SPS: /* SPS */
           if (spsBuf) spsBuf->assign(start + 4, size);
           break;
         case HEVC_NAL_PPS: /* PPS */
           if (ppsBuf) ppsBuf->assign(start + 4, size);
           break;
         case HEVC_NAL_VPS: /* VPS */
           if (vps)
             vps->assign(start + 4, size);
           break;
         default:
            if (nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA_NUT) {
              isKeyframe = true;
            }
            break;
         }
       }
       if (toNal) {
          // 把长度改成NAL头部
          start[0] = 0;
          start[1] = 0;
          start[2] = 0;
          start[3] = 1;
       }
       start += size + 4;
   }
   return true;
}

#define ARRAY_SIZE(X) (sizeof(X)/sizeof(X[0]))

int gAacSampleMap[] = 
{
  96000, 88200, 64000, 48000,
  44100, 32000, 24000, 22050,
  16000, 12000, 11025, 8000,
  7350, 0, 0, 0
};

int AacSampleRate(int nIndex) {
  if (nIndex < (int)ARRAY_SIZE(gAacSampleMap) || nIndex >= 0)
    return gAacSampleMap[nIndex];
  return -1;
}

int AacSampleIndex(int nSoundRate) {
  int nSampleIndex = 0x07;
  for (size_t i = 0; i< ARRAY_SIZE(gAacSampleMap); i++)
  {
    if (gAacSampleMap[i] == nSoundRate){
      nSampleIndex = i;
      break;
    }
  }
  return nSampleIndex;
}

int getAacDecInfo(uint8_t *buff, int* samplerate, int* channel, int* profile)
{
  if (profile)
    *profile = (buff[0] & 0xFC)>>3;
  if (samplerate || channel){
    if (samplerate){
      int idxSample = (buff[0] & 0x07) * 2;
      if (buff[1] & 0x80) idxSample++;
      *samplerate = AacSampleRate(idxSample);
    }
    if (channel)
      *channel = (buff[1] >>3) & 0x0F;
  }
  return 2;
}

int genAacDecInfo(char *buff, int samplerate, int channel, int profile)
{
  PutBitContext pb;
  init_put_bits(&pb, buff, 2);
  put_bits(&pb, 5, profile);    //object type - AAC-LC
  put_bits(&pb, 4, AacSampleIndex(samplerate));
  put_bits(&pb, 4, channel);    // channel configuration
  //GASpecificConfig
  put_bits(&pb, 1, 0);    // frame length - 1024 samples
  put_bits(&pb, 1, 0);    // does not depend on core coder
  put_bits(&pb, 1, 0);    // is not extension
  flush_put_bits(&pb);
  return 2;
}

int genAdtsHeader(uint8_t *buf, int size, int samlerate, int channel, int profile)
{
  PutBitContext pb;

  init_put_bits(&pb, (char*)buf, ADTS_HEADER_SIZE);

  /* adts_fixed_header */
  put_bits(&pb, 12, 0xfff);   /* syncword */
  put_bits(&pb, 1, 0);        /* ID */
  put_bits(&pb, 2, 0);        /* layer */
  put_bits(&pb, 1, 1);        /* protection_absent */
  put_bits(&pb, 2, profile - 1);		/* profile_objecttype */
  put_bits(&pb, 4, AacSampleIndex(samlerate));
  put_bits(&pb, 1, 0);        /* private_bit */
  put_bits(&pb, 3, channel); /* channel_configuration */
  put_bits(&pb, 1, 0);        /* original_copy */
  put_bits(&pb, 1, 0);        /* home */

  /* adts_variable_header */
  put_bits(&pb, 1, 0);        /* copyright_identification_bit */
  put_bits(&pb, 1, 0);        /* copyright_identification_start */
  put_bits(&pb, 13, ADTS_HEADER_SIZE + size); /* aac_frame_length */
  put_bits(&pb, 11, 0x7ff);   /* adts_buffer_fullness */
  put_bits(&pb, 2, 0);        /* number_of_raw_data_blocks_in_frame */

  flush_put_bits(&pb);

  return 0;
}

bool parseAdtsHeader(uint8_t* data, int size, int* samlerate, int* channel, int* profile)
{
  if (size > ADTS_HEADER_SIZE && data[0] == 0xFF && data[1] == 0xF1){
    if (profile){
      *profile = (data[2] >> 6) + 1;
    }
    if (samlerate){
      *samlerate = AacSampleRate(data[2] >> 2 & 0x0F);
    }
    if (channel){
      *channel = ((data[2] & 0x01) << 2) + (data[3] >> 6);
    }
    return true;
  }
  return false;
}

bool HasAdtsHeader(uint8_t* data, int size)
{
  return (size > ADTS_HEADER_SIZE && data[0] == 0xFF && data[1] == 0xF1);
}
