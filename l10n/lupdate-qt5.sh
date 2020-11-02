#!/bin/sh
#
# Update translation files
#

lupdate-qt5 $1 ../src/* -ts gitahead_de.ts
lupdate-qt5 $1 ../src/* -ts gitahead_en.ts
lupdate-qt5 $1 ../src/* -ts gitahead_es.ts
lupdate-qt5 $1 ../src/* -ts gitahead_pt_BR.ts
lupdate-qt5 $1 ../src/* -ts gitahead_zh-CN.ts
