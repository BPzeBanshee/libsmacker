/* see smacker.h */
#include "smacker.h"

/* data structures */
#include "smk_bitstream.h"
#include "smk_hufftree.h"

/* defines NULL... */
#include <stdlib.h>
/* FILE pointers and etc */
#include <stdio.h>
#include <errno.h>
/* memset */
#include <string.h>
/* error handling ("C Exceptions") */
#include <setjmp.h>

/* some flags defines */
#define    SMK_FLAG_RING_FRAME    0x01
#define    SMK_FLAG_Y_INTERLACE    0x02
#define    SMK_FLAG_Y_DOUBLE    0x04

#define SMK_FLAG_KEYFRAME    0x01

#define SMK_FLAG_AUDIO_COMPRESS    (1 << 31)
#define SMK_FLAG_AUDIO_EXISTS    (1 << 30)
#define SMK_FLAG_AUDIO_BITRATE  (1 << 29)
#define SMK_FLAG_AUDIO_STEREO   (1 << 28)
#define SMK_FLAG_AUDIO_V2       (0x03 << 26)

/* GLOBALS */
/* for error handling */
static jmp_buf jb;

/* SMACKER DATA STRUCTURES */
struct smk_palette_t
{
    /* user-switch */
    unsigned char       enable;

    unsigned char       *buffer;
};

struct smk_video_t
{
    unsigned int    w;
    unsigned int    h;

    /* display flags: Y-interlace, Y-double, ring frame */
    unsigned char       flags;

    /* user-switch */
    unsigned char       enable;

    /* Huffman trees */
    struct smk_huff_t   *mmap;
    struct smk_huff_t   *mclr;
    struct smk_huff_t   *full;
    struct smk_huff_t   *type;

    unsigned char       *buffer;
};

struct smk_audio_t
{
    unsigned char       compress;
    unsigned char       exists;
    unsigned char       channels;
    unsigned char       bitdepth;
    unsigned int        rate;

    /* unsigned int        max_buffer; */

    /* user-switch */
    unsigned char       enable;

    void                *buffer;
    unsigned int        buffer_size;
};

struct smk_t
{
    /* meta-info */
    unsigned char    v;
    unsigned char       mode;

    /* total frames, frames per second */
    unsigned int    f;
    float           fps;

    /* Holds per-frame flags (i.e. 'keyframe') */
    unsigned char       *frame_flags;

    /* Holds per-frame types (i.e. 'audio track 3, 2, and palette swap'') */
    unsigned char       *frame_type;

    /* Index of current frame */
    unsigned int        cur_frame;

    /* on-disk mode */
    FILE                *fp;
    unsigned int  *chunk_offset;

    /* in-memory mode: unprocessed chunks */
    unsigned char **chunk_data;

    /* shared */
    unsigned int *chunk_size;

    /* palette */
    struct smk_palette_t  palette;

    /* video */
    struct smk_video_t  video;

    /* audio */
    struct smk_audio_t  audio[7];

};

/* An fread wrapper: consumes N bytes, throws an except
   when size doesn't match expected */
static void read(void *buf, size_t size, FILE *fp)
{
    if (fread(buf,size,1,fp) != 1)
    {
        fprintf(stderr,"libsmacker::read() - ERROR - Short read looking for %lu bytes\n",size);
        longjmp(jb,0);
    }
}

/* And helper functions to do the reading, plus
   byteswap from LE to host order */
static unsigned int smk_read_ui(FILE *fp)
{
    unsigned char buf[4];
    read(buf,4,fp);
    return ((unsigned int) buf[3] << 24) |
        ((unsigned int) buf[2] << 16) |
        ((unsigned int) buf[1] << 8) |
        ((unsigned int) buf[0]);
}

static unsigned char smk_read_uc(FILE *fp)
{
    unsigned char buf;
    read(&buf,1,fp);
    return buf;
}

/* Same as above, except it reads from a RAM buffer */
static unsigned int smk_grab_ui(unsigned char *buf)
{
    return ((unsigned int) buf[3] << 24) |
        ((unsigned int) buf[2] << 16) |
        ((unsigned int) buf[1] << 8) |
        ((unsigned int) buf[0]);
}

/* "Renders" (unpacks) the frame at cur_frame
   Preps all the image and audio pointers */
static void smk_render(smk s)
{
    unsigned char *buffer;
    unsigned char *p,*t;

    unsigned int i,j,k,size;

    /* used for audio decoding */
    struct smk_bit_t *bs;
    char base_8[2];
    short base_16[2];

    short s16;

    struct smk_huff_t *aud_tree[4];

    const unsigned char palmap[64] = {
        0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
        0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
        0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
        0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
        0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
        0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
        0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
        0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
    };

/*       const unsigned short sizetable[64] = {
       1,     2,    3,    4,    5,    6,    7,    8,
       9,    10,   11,   12,   13,   14,   15,   16,
       17,   18,   19,   20,   21,   22,   23,   24,
       25,   26,   27,   28,   29,   30,   31,   32,
       33,   34,   35,   36,   37,   38,   39,   40,
       41,   42,   43,   44,   45,   46,   47,   48,
       49,   50,   51,   52,   53,   54,   55,   56,
       57,   58,   59,  128,  256,  512, 1024, 2048
       }; */

    printf("decoding frame %d\n",s->cur_frame);

    if (s->mode == SMK_MODE_DISK)
    {
        /* In disk-streaming mode: make way for our incoming chunk buffer */
        buffer = malloc(s->chunk_size[s->cur_frame]);

        /* Skip to frame in file */

        if (fseek(s->fp,s->chunk_offset[s->cur_frame],SEEK_SET))
        {
            fprintf(stderr,"libsmacker::smk_render - ERROR: fseek to frame %u (offset %u) failed.\n",s->cur_frame,s->chunk_offset[s->cur_frame]);
            return;
        }

printf("pos in file: %lu\n",ftell(s->fp));
        /* Read into buffer */
        if (s->chunk_size[s->cur_frame] != fread(buffer,1,s->chunk_size[s->cur_frame],s->fp))
        {
            fprintf(stderr,"short read\n");
        }
    } else {
        /* Just point buffer at the right place */
        buffer = s->chunk_data[s->cur_frame];
    }

    p = buffer;

    /* Palette record first */
    if (s->frame_type[s->cur_frame] & 0x01)
    {
        size = 4 * (*p);

        if (s->palette.enable)
        {
            p ++; size --;
            t = malloc(768);
            /* memset(t,0,768); */ /* is this needed? */
            i = 0; /* index into NEW palette */
            j = 0; /* Index into OLD palette */
            while ( (i < 256) && (size > 0) ) /* looping index into NEW palette */
            {
                if ((*p) & 0x80)
                {
                    k = ((*p) & 0x7F) + 1;
                    if (s->palette.buffer != NULL)
                    {
                        memcpy(&t[i*3],&s->palette.buffer[j*3],k * 3);
                        j += k;
                    } else {
                        memset(&t[i*3],0,k*3);
                    }
                    i += k;
                    p ++; size --;
                } else if ((*p) & 0x40) {
                    k = ((*p) & 0x3F) + 1;  /* count */
                    p ++; size --;
                    j = (*p);
                    if (s->palette.buffer != NULL)
                    {
                        memcpy(&t[i*3],&s->palette.buffer[j*3],k * 3);
                        j += k;
                    } else {
                        memset(&t[i*3],0,k*3);
                    }
                    i += k;
                    p ++; size --;
                } else {
                    t[palmap[(i * 3)]] = *p; p++; size --;
                    t[palmap[(i * 3) + 1]] = *p; p++; size --;
                    t[palmap[(i * 3) + 2]] = *p; p++; size --;
                    i ++;
                }
            }

            if (s->palette.buffer != NULL) free (s->palette.buffer);
            s->palette.buffer = t;
        }

        /* advance any remaining unparsed distance */
        p += size;
    }


    /* Unpack audio chunks */
    for (i = 0; i < 7; i ++)
    {
        printf("BEFORE AUDIO [%d]: pointing at %d\n",i,p-buffer);
        if (s->frame_type[s->cur_frame] & (0x02 << i))
        {
            size = smk_grab_ui(p);

            if (s->audio[i].enable)
            {
                p += 4; size -= 4;

                if (s->audio[i].compress)
                {
                    s->audio[i].buffer_size = smk_grab_ui(p);
                    p += 4; size -= 4;

                    t = malloc(s->audio[i].buffer_size);

                    /* Compressed audio: must unpack here */
                    /*  Set up a bitstream */
                    bs = smk_bs_init (p, size);

                    if (smk_bs_1(bs))
                    {
                        printf("COMPRESSED DATA PRESENT\n");

                        if (s->audio[i].channels != (smk_bs_1(bs) == 1 ? 2 : 1))
                        {
                            fprintf(stderr,"libsmacker::smk_render - ERROR: frame %u, audio channel %u, mono/stereo mismatch\n",s->cur_frame,i);
                            return;
                        }
                        if (s->audio[i].bitdepth != (smk_bs_1(bs) == 1 ? 16 : 8))
                        {
                            fprintf(stderr,"libsmacker::smk_render - ERROR: frame %u, audio channel %u, 8-/16-bit mismatch\n",s->cur_frame,i);
                            return;
                        }

                        if (s->audio[i].channels == 1)
                        {
                            if (s->audio[i].bitdepth == 8)
                            {
                                aud_tree[0] = smk_build_tree(bs);
                                aud_tree[1] = NULL;
                                aud_tree[2] = NULL;
                                aud_tree[3] = NULL;
//                                if (s->audio[i].buffer == NULL)
                                ((char *)t)[0] = (char)smk_bs_8(bs);
//((char *)t)[0] = smk_tree_lookup(bs,aud_tree[0]);
 //                               else
 //                                   ((char *)t)[0] = smk_bs_8(bs);
                                j = 1;
                            }/* else {
                                aud_tree[0] = smk_build_tree(bs);
                                aud_tree[1] = smk_build_tree(bs);
                                aud_tree[2] = NULL;
                                aud_tree[3] = NULL;

                                base_16[0] = ((smk_bs_8(bs)) << 8) | (smk_bs_8(bs));
                            }*/
                        } else {
                            if (s->audio[i].bitdepth == 8)
                            {
                                aud_tree[0] = smk_build_tree(bs);
                                if (aud_tree[0] == NULL) { printf("TREE0 NULL\n");}
                                aud_tree[1] = NULL;
                                smk_bs_align(bs);
                                aud_tree[2] = smk_build_tree(bs);
                                if (aud_tree[2] == NULL) { printf("TREE2 NULL\n");}
                                aud_tree[3] = NULL;
                                ((char *)t)[0] = (char)smk_bs_8(bs);
                                ((char *)t)[1] = (char)smk_bs_8(bs);
/*                            } else {
                                aud_tree[0] = smk_build_tree(bs);
                                aud_tree[1] = smk_build_tree(bs);
                                aud_tree[2] = smk_build_tree(bs);
                                aud_tree[3] = smk_build_tree(bs);
                                base_16[1] = ((smk_bs_8(bs)) << 8) | (smk_bs_8(bs));
                                base_16[0] = ((smk_bs_8(bs)) << 8) | (smk_bs_8(bs)); */
                            }
                        }

/* Print the BASES */
/* printf("Bases are: %d %d %d %d\n", base_8[0], base_8[1], base_16[0], base_16[1]); */

printf("Ready to begin consuming data, starting from byte %u bit %u\nBase[0] = %d",bs->byte_num,bs->bit_num,((char *)t)[0]);

/* All set: let's read some DATA! */
                        while (j < s->audio[i].buffer_size)
                        {
                            if (s->audio[i].channels == 1)
                            {
                                if (s->audio[i].bitdepth == 8)
                                {
                                    ((char *)t)[j] = (smk_tree_lookup(bs,aud_tree[0])) + ((char *)t)[j - 1];
                                    j ++;
                                }/* else {
                                    s16 = (256 * smk_tree_lookup(bs,aud_tree[0])) + smk_tree_lookup(bs,aud_tree[1]);
                                    s16 += base_16[0];
                                    t[j] = s16 % 256;
                                    j ++;
                                    t[j] = s16 / 256;
                                    j ++;
                                } */
                            } else {
                                if (s->audio[i].bitdepth == 8)
                                {
                                    ((char *)t)[j] = (smk_tree_lookup(bs,aud_tree[0])) + ((char *)t)[j - 1];
                                    j ++;
                                    ((char *)t)[j] = (smk_tree_lookup(bs,aud_tree[2])) + ((char *)t)[j - 2];
                                    j ++;
                                } /* else {
                                    s16 = (256 * smk_tree_lookup(bs,aud_tree[2])) + smk_tree_lookup(bs,aud_tree[3]);
                                    s16 += base_16[1];
                                    t[j] = s16 % 256;
                                    j ++;
                                    t[j] = s16 / 256;
                                    j ++;
                                } */
                            }
                        }
                    }

                    p += (bs->byte_num + 1);
                    size -= (bs->byte_num + 1);

                    /* All done with the trees, free them. */
                    if (aud_tree[0] != NULL) smk_del_huffman(aud_tree[0]);
                    if (aud_tree[1] != NULL) smk_del_huffman(aud_tree[1]);
                    if (aud_tree[2] != NULL) smk_del_huffman(aud_tree[2]);
                    if (aud_tree[3] != NULL) smk_del_huffman(aud_tree[3]);

                    /* All done with the bitstream, free it. */
                    free(bs);
                } else {
                    s->audio[i].buffer_size = size;
                    t = malloc(s->audio[i].buffer_size);

                    memcpy(t,p,size);
                    p += size;
                    size = 0;
                }

                if (s->audio[i].buffer != NULL) free (s->audio[i].buffer);

                s->audio[i].buffer = t;
            } else {
                printf("There was an audio track %d here, but I have it disabled.\n",i);
            }

            /* advance any remaining unparsed distance */
printf("Well, still got %u bytes left, seeking\n",size);
            p += size;
        }
    }

    printf("BEFORE VIDEO: pointing at %d\n",p-buffer);

    if (s->mode == SMK_MODE_DISK)
    {
        /* Remember that buffer we allocated?  Trash it */
        free(buffer);
    }
}

/* PUBLIC FUNCTIONS */
/* open an smk (from a file) */
smk smk_open(const char* fn, unsigned char m)
{
    /* sadly we must declare this volatile */
    volatile smk s;

    size_t retval;
    int temp_i;
    unsigned int temp_u;
    unsigned char b[3];
    unsigned char *hufftree_chunk;

    struct smk_bit_t *bs;

    /* make some temp arrays for holding things */
    unsigned int tree_size;

    /* unsigned int *chunk_size; */
    /* unsigned char     *s->frame_type; */

    FILE *fp = fopen(fn,"rb");
    if (fp == NULL)
    {
        fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: could not open file (errno: %d)\n",fn,errno);
        perror ("    Error reported was: ");
        return NULL;
    }

    s = (smk) malloc (sizeof (struct smk_t));
    if (s == NULL)
    {
        fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: Unable to allocate %lu bytes for smk struct\n",fn,sizeof(struct smk_t));
        fclose(fp);
        return NULL;
    }
    memset(s,0,sizeof(struct smk_t));

    /* Set the Mode value */
    s->mode = (m == SMK_MODE_DISK ? SMK_MODE_DISK : SMK_MODE_MEMORY);

    /* let's read! */
    if (!setjmp(jb))
    {
        /* Check for a valid signature */
        retval = fread(b,1,3,fp);
        if (retval != 3 || (memcmp(b,"SMK",3) ))
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: invalid SMKn signature\n",fn);
            longjmp(jb,0);
        }

        /* Read .smk file version */
        s->v = smk_read_uc(fp);
        if (s->v != '2' && s->v != '4')
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: invalid SMK version %u (expected: 2 or 4)\n",fn,s->v);
            longjmp(jb,0);
        }

        /* width, height, total num frames */
        s->video.w = smk_read_ui(fp);
        s->video.h = smk_read_ui(fp);

        s->f = smk_read_ui(fp);

        /* frames per second calculation */
        temp_i = (int)smk_read_ui(fp);
        if (temp_i > 0)
            s->fps = 1000.0f / temp_i;
        else if (temp_i < 0)
            s->fps = -100000.0f / temp_i;
        else
            s->fps = 10.0f;

        temp_u = smk_read_ui(fp);
        s->video.flags = temp_u % 256;

        /* Skip over these "unpacked" sizes, they are specific to
           the official smackw32.dll usage */
        if (fseek(fp,28,SEEK_CUR))
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: audio-buff size not skipped OK.\n",fn);
            longjmp(jb,0);
        }

        /* for (temp_i = 0; temp_i < 7; temp_i ++)
        {
            s->audio[temp_i].max_buffer = smk_read_ui(fp);
        } */

        tree_size = smk_read_ui(fp);

        /* Skip over these "unpacked" sizes, they are specific to
           the official smackw32.dll usage */
        if (fseek(fp,16,SEEK_CUR))
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: huff-table size not skipped OK.\n",fn);
            longjmp(jb,0);
        }

        /* read audio rate data */
        for (temp_i = 0; temp_i < 7; temp_i ++)
        {
            temp_u = smk_read_ui(fp);
            s->audio[temp_i].compress = ((temp_u & SMK_FLAG_AUDIO_COMPRESS) ? 1 : 0);
            s->audio[temp_i].exists = ((temp_u & SMK_FLAG_AUDIO_EXISTS) ? 1 : 0);
            s->audio[temp_i].bitdepth = ((temp_u & SMK_FLAG_AUDIO_BITRATE) ? 16 : 8);
            s->audio[temp_i].channels = ((temp_u & SMK_FLAG_AUDIO_STEREO) ? 2 : 1);
            if ((temp_u & SMK_FLAG_AUDIO_V2) != 0)
            {
                fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: audio track %d is compressed with Bink (perceptual) Audio Codec: this is currently unsupported by libsmacker\n",fn,temp_i);
                longjmp(jb,0);
            }
            s->audio[temp_i].rate = (temp_u & 0x00FFFFFF);
        }

        /* Skip over Dummy field */
        if (fseek(fp,4,SEEK_CUR))
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: dummy value not skipped OK.\n",fn);
            longjmp(jb,0);
        }

        /* Onto FrameSizes array */
        s->frame_flags = malloc(s->f);

        s->chunk_size = malloc(s->f * 4);
        for (temp_u = 0; temp_u < s->f; temp_u ++)
        {
            s->chunk_size[temp_u] = smk_read_ui(fp);

            if (s->chunk_size[temp_u] & SMK_FLAG_KEYFRAME)
                s->frame_flags[temp_u] = SMK_FLAG_KEYFRAME;
            else
                s->frame_flags[temp_u] = 0;
            s->chunk_size[temp_u] &= 0xFFFFFFFC;
        }

        /* That was easy... FrameTypes! */
        s->frame_type = malloc(s->f);
        for (temp_u = 0; temp_u < s->f; temp_u ++)
        {
            s->frame_type[temp_u] = smk_read_uc(fp);
        }

        /* HuffmanTrees
           We know the sizes already: read and assemble into
           something actually parse-able at run-time */
        hufftree_chunk = malloc(tree_size);
        retval = fread(hufftree_chunk,1,tree_size,fp);
        if (retval != tree_size)
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: short read on hufftree block (wanted %u, got %lu)\n",fn,tree_size,retval);
            longjmp(jb,0);
        }

        /* set up a Bitstream */
        bs = smk_bs_init(hufftree_chunk, tree_size);
        /* create some tables */
        s->video.mmap = NULL; /* smk_build_tree(bs); */
        s->video.mclr = NULL; /* smk_build_tree(bs); */
        s->video.type = NULL; /* smk_build_tree(bs); */
        s->video.full = NULL; /* smk_build_tree(bs); */

        /* Read in the rest of the data. */
        /*   For MODE_MEMORY, read the chunks and store */
        if (s->mode == SMK_MODE_MEMORY)
        {
            s->chunk_data = (unsigned char **)malloc(s->f * sizeof(unsigned char*));
            for (temp_u = 0; temp_u < s->f; temp_u ++)
            {
                s->chunk_data[temp_u] = (unsigned char *)malloc(s->chunk_size[temp_u]);
                retval = fread(s->chunk_data[temp_u],1,s->chunk_size[temp_u],fp);
                if (retval != s->chunk_size[temp_u])
                {
                    fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: short read on chunk_data block[%u] (wanted %u, got %lu)\n",fn,temp_u,s->chunk_size[temp_u],retval);
                    longjmp(jb,0);
                }
            }
        } else {
            /* MODE_DISK: don't read anything now, just precompute offsets. */
            /*   use fseek to verify that the file is "complete" */
            s->chunk_offset = (unsigned int *)malloc(s->f * sizeof(unsigned int));
            for (temp_u = 0; temp_u < s->f; temp_u ++)
            {
                s->chunk_offset[temp_u] = ftell(fp);
                if (fseek(fp,s->chunk_size[temp_u],SEEK_CUR))
                {
                    fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: fseek to frame %u not OK.\n",fn,temp_u);
                    longjmp(jb,0);
                }
            }
        }

        /* guess that's it... try reading one more byte just to see if there was anything left to read */
        retval = fread(b,1,1,fp);
        if (retval != 0 || !feof(fp))
        {
            fprintf(stderr,"libsmacker::smk_open(%s) - ERROR: still data left to read at position %lu\n",fn,ftell(fp));
            longjmp(jb,0);
        }

        /* All done, do what you will with the file pointer */
        if (s->mode == SMK_MODE_DISK)
            s->fp = fp;
        else
            fclose(fp);
    } else {
        fprintf(stderr,"libsmacker::smk_open(%s) - Errors encountered, bailing out\n",fn);
        smk_close(s);
        s = NULL;

        fclose(fp);
    }

    return s;
}

/* close out an smk file and clean up memory */
void smk_close(smk s)
{
    unsigned int u;

    if (s != NULL)
    {
        if (s->palette.buffer != NULL) free(s->palette.buffer);
        if (s->video.buffer != NULL) free (s->video.buffer);
        for (u=0; u<7; u++)
        {
            if (s->audio[u].buffer != NULL) free (s->audio[u].buffer);
        }

        /* Huffman trees */
        if (s->video.mmap != NULL) smk_del_huffman(s->video.mmap);
        if (s->video.mclr != NULL) smk_del_huffman(s->video.mclr);
        if (s->video.full != NULL) smk_del_huffman(s->video.full);
        if (s->video.type != NULL) smk_del_huffman(s->video.type);

        if (s->frame_flags != NULL) free(s->frame_flags);
        if (s->frame_type != NULL) free(s->frame_type);

        /* disk-mode */
        if (s->fp != NULL) fclose(s->fp);
        if (s->chunk_offset != NULL) free(s->chunk_offset);

        /* mem-mode */
        if (s->chunk_data != NULL)
        {
            for (u=0; u<s->f; u++)
            {
                if (s->chunk_data[u] != NULL) free(s->chunk_data[u]);
            }
            free(s->chunk_data);
        }

        if (s->chunk_size != NULL) free(s->chunk_size);

        free(s);
    }
}

/* enable/disable decode features */
void smk_enable_palette(smk s, unsigned char v) { s->palette.enable = (v == 0 ? 0 : 1); }
void smk_enable_video(smk s, unsigned char v) { s->video.enable = (v == 0 ? 0 : 1); }
void smk_enable_audio(smk s, unsigned char t, unsigned char v) { s->audio[t].enable = (v == 0 ? 0 : 1); }

/* tell some info about the file */
unsigned int smk_info_video_w(smk s) { return s->video.w; }
unsigned int smk_info_video_h(smk s) { return s->video.h; }
unsigned int smk_info_f(smk s) { return s->f; }
float        smk_info_fps(smk s) { return s->fps; }

unsigned int smk_info_cur_frame(smk s) { return s->cur_frame; }

/* get info about audio tracks */
/* returns a BYTE with bitfields set, indicating presence of
   audio for each of 7 tracks */
unsigned char smk_info_audio_tracks(smk s)
{
    return ( ( (s->audio[0].exists) << 0 ) |
             ( (s->audio[1].exists) << 1 ) |
             ( (s->audio[2].exists) << 2 ) |
             ( (s->audio[3].exists) << 3 ) |
             ( (s->audio[4].exists) << 4 ) |
             ( (s->audio[5].exists) << 5 ) |
             ( (s->audio[6].exists) << 6 ) );
}

unsigned char smk_info_audio_channels(smk s, unsigned char t) { return s->audio[t].channels; }
unsigned char smk_info_audio_bitdepth(smk s, unsigned char t) { return s->audio[t].bitdepth; }
unsigned int smk_info_audio_rate(smk s, unsigned char t) { return s->audio[t].rate; }

unsigned char * smk_get_palette(smk s) { return s->palette.buffer; }
unsigned char * smk_get_video(smk s) { return s->video.buffer; }
unsigned char * smk_get_audio(smk s, unsigned char t) { return s->audio[t].buffer; }
unsigned int smk_get_audio_size(smk s, unsigned char t) { return s->audio[t].buffer_size; }

/* rewind to first frame and unpack */
int smk_first(smk s)
{
    s->cur_frame = 0;
    smk_render(s);

    if (s->f == 1) return SMK_LAST;
    return SMK_MORE;
}

/* advance to next frame */
int smk_next(smk s)
{
    if (s->cur_frame + 1 < s->f)
    {
        s->cur_frame ++;
        smk_render(s);
        if (s->cur_frame + 1 == s->f) return SMK_LAST;
        return SMK_MORE;
    }
    return SMK_DONE;
}

/* seek to a keyframe in an smk */
int smk_seek_keyframe(smk s, unsigned int f)
{
    /* rewind (or fast forward!) exactly to f */
    s->cur_frame = f;

    /* roll back to previous keyframe in stream, or 0 if no keyframes exist */
    while (s->cur_frame > 0 && !(s->frame_flags[s->cur_frame] & SMK_FLAG_KEYFRAME)) s->cur_frame --;

    /* render the frame: we're ready */
    smk_render(s);

    /* return "actual seek position" */
    return s->cur_frame;
}

/* seek to an exact frame in an smk */
int smk_seek_exact(smk s, unsigned int f)
{
    smk_seek_keyframe(s,f);
    while (s->cur_frame < f) smk_next(s);
    return s->cur_frame;
}
