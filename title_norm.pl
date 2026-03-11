#!/usr/bin/perl
use strict;
use warnings;
use Data::Dumper;
my @props=qw/at in on of by with an a to between among/;
my %props=map{$_=>1} @props;
while(<>) {
  my $line = s/^\s*(\S.*\S)\s*$/$1/r;
  #$line =~ s/\\p//r;
  $line =~ s/[[:punct:]]//g;
  $line =~ s/\p{P}//g;
  $line =~ s/—//g;
  $line =~ s/\s+/_/g;
  my ($car, @cdr)=map {s/^(.)(.*)$/\U$1\E\L$2\E/r} split /\s+/, $line;
  @cdr=map {
    my $lc=qq/\L$_\E/; 
    if (exists $props{$lc}){$lc}else{$_}
  }@cdr;
  print join qq/ /, ($car, @cdr);
  print "\n";
}
