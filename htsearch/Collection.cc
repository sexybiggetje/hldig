//
// Collection.cc
//
// Collection: Specifies a list of databases to use in the search
//
// Part of the ht://Dig package   <http://www.htdig.org/>
// Copyright (c) 1995-2000 The ht://Dig Group
// For copyright details, see the file COPYING in your distribution
// or the GNU Public License version 2 or later
// <http://www.gnu.org/copyleft/gpl.html>
//
// $Id: Collection.cc,v 1.2 2002/02/02 18:18:13 ghutchis Exp $
//

#ifdef HAVE_CONFIG_H
#include "htconfig.h"
#endif /* HAVE_CONFIG_H */

#include "htsearch.h"
#include "Collection.h"
#include "ResultMatch.h"
#include "WeightWord.h"
#include "StringMatch.h"
#include "QuotedStringList.h"
#include "URL.h"
#include "HtURLCodec.h"

#include <fstream.h>
#include <stdio.h>
#include <ctype.h>
#include <syslog.h>
#include <locale.h>

//*****************************************************************************
//
Collection::Collection(const char *name, const char *word_file, 
                       const char *index_file, const char *doc_file, 
                       const char *doc_excerpt)
{
    collectionName = name;
    wordFile = word_file;
    indexFile = index_file;
    docFile = doc_file;
    docExcerpt = doc_excerpt;
    matches = NULL;
    searchWords = NULL;
    searchWordsPattern = NULL;
    isopen = 0;
}

Collection::~Collection()
{
  if(matches) delete matches;
  if(searchWords) delete searchWords;
  if(searchWordsPattern) delete searchWordsPattern;
  Close();
}

void
Collection::Open()
{
    if (!isopen)
    {
        docDB.Read(docFile, indexFile, docExcerpt);
    }
    isopen = 1;
}

void
Collection::Close()
{
    if (isopen)
    {
        docDB.Close();
    }
    isopen = 0;
}

DocumentRef *
Collection::getDocumentRef(int id)
{
    Open();
    return docDB[id];
}

int 
Collection::ReadExcerpt(DocumentRef &ref) 
{ 
    Open();
    return docDB.ReadExcerpt(ref); 
}
 
