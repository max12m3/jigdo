#! /usr/bin/env awk -f
#  __   _
#  |_) /|  Copyright (C) 2001-2003  |  richard@
#  | \/�|  Richard Atterer          |  atterer.net
#  � '` �
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License, version 2. See
#  the file COPYING for details.

# Syntax: glade-filter some/path/gtk-interface

# where some/path/gtk-interface.cc.tmp was generated by glade. Will
# post-process that file to generate a simple class that contains
# pointers to all the widgets, and the create_ functions, so you don't
# need to look up widgets by name.

# Overwrites some/path/gtk-interface.cc and some/path/gtk-interface.hh
#______________________________________________________________________

# Add code line to hh, with right indentation
function addhh(string) {
  sub(/^ +/, "", string);
  if (substr(string, 1, 1) == "}") hhindent = substr(hhindent, 3);
  string = hhindent string;
  sub(/ +$/, "", string);
  hh = hh string "\n";
  if (substr(string, length(string)) == "{") hhindent = hhindent "  ";
}

function addcc(string) {
  cc = cc string "\n";
}
#______________________________________________________________________

BEGIN {
  if (ARGC != 2) {
    print "Syntax: glade-filter.awk some/path/gtk-interface";
    exit 1;
  }
  stem = ARGV[1];
  leaf = stem; gsub(/.*\//, "", leaf);
  outputcc = stem".cc";
  outputhh = stem".hh";
  hhGuard = toupper(outputhh);
  gsub(/.*\//, "", hhGuard);
  gsub(/[^A-Z0-9]/, "_", hhGuard);
  namespace = "GUI";

  cc = "// Automatically created from `"leaf".cc.tmp' by glade-filter.awk\n\n";
  hh = cc;
  #addcc("#include <"outputhh">");
  addhh("#ifndef "hhGuard);
  addhh("#define "hhGuard);
  addhh("");
  addhh("#include <config.h>");
  addhh("#include <gtk/gtk.h>");
  addhh("");
  addhh("namespace "namespace" {");
  collectDecl = 0;

  ARGV[1] = stem".cc.tmp";
}
#______________________________________________________________________

/^#include / {
  sub(/ "/, " <");
  sub(/"$/, ">");
  sub(/\.cc?\.tmp/, ".cc");
  sub(/\.hh?\.tmp/, ".hh");
}

#/@VERSION@/ {
#  gsub(/@VERSION@/, version);
#}

# Fixup #defines for GLADE_HOOKUP_OBJECT[_NO_REF] *not* to set the
# name string - I never use lookup_widget(), so no need to waste space
# for all the strings
/g_object_set_data_full \(G_OBJECT \(component\), name,|g_object_set_data \(G_OBJECT \(component\), name, widget\)/ {
  sub(/, name,/, ", \"\",");
}

/^create_[a-zA-Z]+ *\( *(void *)?\) *$/ {
  # New function definition => new GUI class
  guiElem = toupper(substr($0, 8, 1)) substr($0, 9, index($0, " ") - 9);
  addcc(namespace"::"guiElem"::create()");
  addhh("");
  addhh("struct "guiElem" {");
  addhh(prevLine" create();");
  collectDecl = 1;
  next;
}

/^ *} *$/ {
  if (length(guiElem) != 0) {
    # End of function definition
    addhh("};");
    guiElem = "";
  }
}

# Weed out decls for vars whose names are likely to have been
# generated by glade: All lowercase, ends with >=1 digits. These
# remain local vars of the create() function, rather than becoming
# members of the struct
/^ *[A-Z][a-zA-Z0-9_]*( +| *\* *)[a-z]+[0-9]+ *(= *[a-zA-Z0-9_]+ *)?; *$/ {
  if (collectDecl) {
    # leave in cc, don't add to hh
    addcc($0);
    next;
  }
}

# "GtkWidget *jigdo_abortButton;" or "GSList *radiobutton1_group = NULL;"
/^ *[A-Z][a-zA-Z0-9_]*( +| *\* *)[a-z][a-zA-Z0-9_]+ *(= *[a-zA-Z0-9_]+ *)?; *$/ {
  if (collectDecl) {
    # GtkSomething declaration; - remove from cc, add to hh inside
    # class. If var contains initializer, only put that in cc
    if (index($0, "=")) {
      l = $0;
      sub(/^ *[A-Z][a-zA-Z0-9_]*( +| *\* *)/, "", l);
      addcc(l);
      sub(/=[^;]*/, "", $0);
    }
    addhh($0);
    next;
  }
}

/(^ *$)|=/ { collectDecl = 0; }

{
  cc = cc $0 "\n"; # By default, copy over code into output file
  prevLine = $0;
}
#______________________________________________________________________

END {
  if (!outputcc) exit 1;

  addhh("");
  addhh("} // namespace "namespace);
  addhh("");
  addhh("#endif /* "hhGuard" */");

  printf("%s", cc) > outputcc;
  printf("%s", hh) > outputhh;

  # To avoid recompilation, only update .cc if contents changed
  #old = "";
  #while ((getline line < outputcc) == 1) old = old line "\n";
  #if (old == cc) print "`"outputcc"' is unchanged";
  #else printf("%s", cc) > outputcc;

  # To avoid recompilation, only update .hh if contents changed
  #old = "";
  #while ((getline line < outputhh) == 1) old = old line "\n";
  #if (old == hh) print "`"outputhh"' is unchanged";
  #else printf("%s", hh) > outputhh;
}