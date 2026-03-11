#!/usr/bin/perl
use strict;
use warnings;
use Getopt::Std;
use POSIX qw(strftime);

my $usage = "Usage: $0 [-n N] [-s since] [-c top_commit] [-f filter] <dir>\n"
          . "  -n N:          number of latest commits to show (default: 10)\n"
          . "  -s since:      only check files modified since this date (e.g. '2025-01-01', '3 months ago')\n"
          . "  -c top_commit: branch/commit/tag as the latest commit (default: HEAD)\n"
          . "  -f filter:     regex on commit messages; prefix with ! to negate\n"
          . "  dir:           directory to scan\n";

my %opts;
getopts('n:s:c:f:h', \%opts) or die $usage;
die $usage if $opts{h};

my $N          = $opts{n} // 10;
my $since      = $opts{s} // "";
my $top_commit = $opts{c} // "HEAD";
my $filter     = $opts{f} // "";

die $usage unless @ARGV == 1;
my $dir = $ARGV[0];

die "Error: '$dir' is not a directory\n" unless -d $dir;
die "Error: N must be a positive integer\n" unless $N =~ /^\d+$/ && $N > 0;

chomp(my $rev_check = `git rev-parse --verify "$top_commit^{commit}" 2>/dev/null`);
die "Error: invalid revision '$top_commit'\n" unless $? == 0 && length($rev_check);

# Parse filter
my $negate_filter = 0;
my $filter_re;
if (length $filter) {
    if ($filter =~ s/^!//) {
        $negate_filter = 1;
    }
    $filter_re = qr/$filter/;
}

# Parse since to epoch
my $since_ts = 0;
if (length $since) {
    chomp(my $ts = `date -d '$since' '+%s' 2>/dev/null`);
    die "Error: invalid date '$since'\n" if $? != 0 || !length($ts);
    $since_ts = $ts + 0;
}
my $threshold = $since_ts;

# Commit message cache
my %msg_cache;
sub get_msg {
    my ($cid) = @_;
    unless (exists $msg_cache{$cid}) {
        chomp($msg_cache{$cid} = `git log -1 --format='%s' $cid 2>/dev/null`);
    }
    return $msg_cache{$cid};
}

sub check_filter {
    my ($msg) = @_;
    return 1 unless defined $filter_re;
    if ($negate_filter) {
        return $msg !~ $filter_re ? 1 : 0;
    } else {
        return $msg =~ $filter_re ? 1 : 0;
    }
}

# Phase 1: build file list sorted by recency
warn "Phase 1: scanning file recency...\n";

my @file_list;  # [timestamp, filepath]

if (length $since) {
    # Efficient: git log --since only walks recent history
    open my $fh, '-|', 'git', 'log', "--since=$since", $top_commit,
        '--format=%at', '--name-only', '--', $dir
        or die "git log: $!\n";

    my %max_ts;
    my $ts = 0;
    while (<$fh>) {
        chomp;
        if (/^(\d+)$/) {
            $ts = $1 + 0;
        } elsif (length $_) {
            $max_ts{$_} = $ts if !exists $max_ts{$_} || $ts > $max_ts{$_};
        }
    }
    close $fh;

    @file_list = map { [$max_ts{$_}, $_] }
                 sort { $max_ts{$b} <=> $max_ts{$a} } keys %max_ts;
} else {
    # List all tracked files at top_commit
    open my $fh, '-|', 'git', 'ls-tree', '-r', '--name-only', $top_commit, '--', $dir
        or die "git ls-tree: $!\n";
    my @files;
    while (<$fh>) {
        chomp;
        push @files, $_;
    }
    close $fh;

    # Get per-file latest timestamp
    for my $f (@files) {
        chomp(my $ts = `git log -1 --format='%at' '$top_commit' -- '$f' 2>/dev/null`);
        push @file_list, [$ts + 0, $f] if length $ts;
    }
    @file_list = sort { $b->[0] <=> $a->[0] } @file_list;
}

my $total = scalar @file_list;
warn "Phase 1 done: $total files\n";

# Phase 2: blame files in recency order, prune by threshold, apply filter
warn "Phase 2: collecting blame data with pruning...\n";
my $processed = 0;
my $skipped   = 0;

# Global top-N: array of [timestamp, commit_id, author]
my @top_n;

for my $entry (@file_list) {
    my ($file_ts, $file_path) = @$entry;

    # Prune: skip file if its latest commit is older than current threshold
    if ($threshold > 0 && $file_ts < $threshold) {
        $skipped++;
        next;
    }

    $processed++;

    # Blame at top_commit, filter lines by threshold
    open my $bfh, '-|', 'git', 'blame', '--porcelain', $top_commit, '--', $file_path
        or next;

    my ($commit, $author, $timestamp);
    my %file_entries;  # commit_id -> [timestamp, commit_id, author]

    while (<$bfh>) {
        chomp;
        if (/^([0-9a-f]{40})\s/) {
            $commit = $1;
        } elsif (/^author (.*)/) {
            $author = $1;
        } elsif (/^author-time (\d+)/) {
            $timestamp = $1 + 0;
        } elsif (/^author-tz /) {
            if (defined $commit && $commit !~ /^0+$/ && $timestamp > $threshold) {
                if (!exists $file_entries{$commit} || $timestamp > $file_entries{$commit}[0]) {
                    $file_entries{$commit} = [$timestamp, $commit, $author];
                }
            }
        }
    }
    close $bfh;

    next unless %file_entries;

    # Merge with global top-N
    my %seen;
    my @merged;

    # Combine existing top-N and new entries, dedup by commit_id
    for my $e (@top_n, values %file_entries) {
        next if $seen{$e->[1]}++;
        push @merged, $e;
    }

    # Sort by timestamp desc
    @merged = sort { $b->[0] <=> $a->[0] } @merged;

    # Apply filter and keep top N
    @top_n = ();
    for my $e (@merged) {
        my $msg = get_msg($e->[1]);
        if (check_filter($msg)) {
            push @top_n, $e;
            last if @top_n >= $N;
        }
    }

    # Update threshold if we have N entries
    if (@top_n >= $N) {
        my $new_thr = $top_n[-1][0];
        $threshold = $new_thr if $new_thr > $threshold;
    }
}

warn "Phase 2 done: processed=$processed skipped=$skipped (total=$total)\n";

# Phase 3: format output with commit messages
for my $e (@top_n) {
    my ($ts, $cid, $auth) = @$e;
    my $dt  = strftime('%Y-%m-%d %H:%M:%S', localtime($ts));
    my $msg = get_msg($cid);
    print "$cid\t$dt\t$auth\t$msg\n";
}
