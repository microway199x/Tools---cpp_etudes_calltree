#!/usr/bin/perl
use strict;
use warnings;
use Data::Dumper;


my ($fragment, $pipeline, $operator, $opid)=(undef)x6;

my %opcost=();
my @uniq_keys=();
my $plan={};

sub norm_time($) {
  my $t = shift;
  my %unit=(ns=>1000000, us=>1000, ms=>1);

  if ($t=~/^(\d+(?:\.\d+)?)(ns|us|ms)$/){
    return ($1+0.0)/$unit{$2};
  }
  elsif ($t=~/^(\d+)s(\d+)ms$/) {
    return ($1+0)*1000+$2;
  }
  elsif ($t=~/^(\d+)m$/){
    return (($1+0)*60)*1000;
  }
  elsif ($t=~/^(\d+)m(\d+)s$/){
    return (($1+0)*60 +$2)*1000;
  }
  else {
    print "\$t=$t\n";
    die "undefined time format!";
    return undef;
  }
}

sub norm_num($) {
  my $n = shift;
  my %unit=(B=>1000000000, M=>1000000, K=>1000);
  if ($n=~/^(\d+(?:\.\d+)?)(B|M|K)$/) {
    return ($1+0.0)*$unit{$2};
  } elsif ($n=~/^\d+$/) {
    return $n+0;
  } else {
    die "undefined number format! '$n'";
    return undef;
  }
}

sub norm_bytes($) {
  my $n = shift;
  my %unit=(B=>1,KB=>1024,MB=>1048576, GB=>1073741824, TB=>1099511627776);
  if ($n=~/^(\d+(?:\.\d+)?)\s*(B|KB|MB|GB|TB)$/) {
    return ($1+0.0)*$unit{$2}/1024/1024;
  } elsif ($n=~/^\d+$/) {
    return ($n+0)/1024/1024;
  } else {
    die "undefined number format! '$n'";
    return undef;
  }
}

while(<>) {
  if (/Fragment\s+(\d+)/){
    $fragment = "Fragment($1)";
    $plan->{$fragment}{id}=$fragment;
    next;
  }
  if (/Pipeline\s+\(id=(\d+)\)/) {
    $pipeline = "Pipeline($1)";
    $plan->{$fragment}{pipelines}{$pipeline}{id}=join "_", ($fragment, $pipeline);
    next;
  }
 
  if (/InstanceNum:\s+(\d+)/) {
    $plan->{$fragment}{InstanceNum}=$1+0;
    next;
  }

  if (/InstancePeakMemoryUsage:\s+(.*)$/) {
    $plan->{$fragment}{InstancePeakMemoryUsage}=norm_bytes($1);
    next;
  }

  if (/(\w+)\s+\(plan_node_id=(\d+)\):/){
    $operator = "$1($2)";
    $opid="plan_node_id=$2";
    next;
  }
  if (/(\w+)\s+\(pseudo_plan_node_id=(-\d+)\):/){
    $operator = "$1($2)";
    $opid="pseudo_plan_node_id=$2";
    next;
  }


  if (/DegreeOfParallelism:\s+(\d+)/) {
    $plan->{$fragment}{pipelines}{$pipeline}{DegreeOfParallelism}=$1+0;
    next;
  }

  if (/(ActiveTime|PendingTime|InputEmptyTime|FirstInputEmptyTime|FollowupInputEmptyTime|OutputFullTime|PreconditionBlockTime|DriverTotalTime|__MAX_OF_DriverTotalTime):\s+(\S+)/) {
    $plan->{$fragment}{pipelines}{$pipeline}{$1}=norm_time($2);
    next;
  }

  if (/(LocalRfWaitingSet|ScheduleAccumulatedChunkMoved|ScheduleAccumulatedRowsPerChunk|ScheduleCounter|ScheduleEffectiveCounter):\s+(\d+)/){
    $plan->{$fragment}{pipelines}{$pipeline}{$1}=$2;
    next;
  }

  if (/(?:Pull|Push)TotalTime:\s+(\S+)/) {
    if (!defined($operator)){
      next;
    }

    my $uniq_key = join "_", ($fragment, $pipeline, $operator);

    if (!exists $opcost{$uniq_key}) {
      $opcost{$uniq_key} = 0;
      $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{op}=$operator;
      $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{id}=$uniq_key;
      $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{opid}=$opid;
    }
    $opcost{$uniq_key}+=norm_time($1);
  }

  if (/(PullChunkNum|PushChunkNum|PullRowNum|PushRowNum):\s+(\S+)/) {
    if (!defined($operator)){
      next;
    }
    my $uniq_key = join "_", ($fragment, $pipeline, $operator);
    $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{$1}=norm_num($2);
  }
  if (/(PullTotalTime|PushTotalTime|OperatorTotalTime):\s+(\S+)/) {
    if (!defined($operator)){
      next;
    }
    my $uniq_key = join "_", ($fragment, $pipeline, $operator);
    $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{$1}=norm_time($2);
  }
  if (/(OperatorPeakMemoryUsage):\s*(.*)/) {
    if (!defined($operator)){
      next;
    }
    my $uniq_key = join "_", ($fragment, $pipeline, $operator);
    $plan->{$fragment}{pipelines}{$pipeline}{ops}{$uniq_key}{$1}=norm_bytes($2);
  }
}

my @opcost=sort {$a->[1] <=> $b->[1]} map{[$_,$opcost{$_}]} keys %opcost;
#print join ("\n", map {sprintf("%0.3f", $_->[1])."\t". $_->[0]} @opcost);
#print "\n";
my $fragments=[values %$plan];
my $pipelines=[map {values %{$_->{pipelines}}} @$fragments];
my $ops=[map {values %{$_->{ops}}} @$pipelines];

sub prune{
  my $items = shift;
  [map {my %obj = %$_;  my %new_obj=map {($_->[0], $_->[1])} grep{ref($_->[1]) eq ""} map{[$_, $obj{$_}] }  keys %obj; \%new_obj}  @$items];
}
#$fragments=prune($fragments);
#$pipelines=prune($pipelines);
#$ops=prune($ops);
#print Dumper($ops); 
my @index = (@$fragments, @$pipelines, @$ops);
my $idx=$ENV{index};

my @selected = grep{exists $_->{$idx}} @index;
#@selected = map {[$_->{id},$_->{$idx}]} grep {my @hjprobeOps = grep {/HASH_JOIN_PROB/} keys %{$_->{ops}}; scalar(@hjprobeOps)>0} @selected;
@selected = map {[$_->{id},$_->{$idx}]} @selected;


print map {qq/$_->[1] \t $_->[0]\n/} sort{$a->[1] <=> $b->[1]} @selected;

