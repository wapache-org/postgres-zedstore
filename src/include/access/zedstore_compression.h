/*
 * zedstore_compression.h
 *		internal declarations for ZedStore compression
 *
 * Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/zedstore_compression.h
 */
#ifndef ZEDSTORE_COMPRESSION_H
#define ZEDSTORE_COMPRESSION_H

extern int zs_try_compress(const char *src, char *dst, int srcSize, int dstCapacity);
extern void zs_decompress(const char *src, char *dst, int compressedSize, int uncompressedSize);

#endif							/* ZEDSTORE_COMPRESSION_H */
