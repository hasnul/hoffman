#!/usr/bin/perl
#
# This script writes control files for 'standard' tablebases (i.e, no
# pruning or move restrictions) into the current directory.
#
# Pass in the XML filenames you wish to generate.  All dependencies
# will be generated as well.
#
# To get the older behavior (write all 5-piece control files and their
# dependencies), call it as 'genctlfile.pl kppkp.xml'
#
# by Brent Baccala; no rights reserved

my $pieces = "qrbnp";

my @pieces = ('q', 'r', 'b', 'n', 'p');
my @non_pawn_pieces = ('q', 'r', 'b', 'n');

my %pieces;
$pieces{q} = 'queen';
$pieces{r} = 'rook';
$pieces{b} = 'bishop';
$pieces{n} = 'knight';
$pieces{p} = 'pawn';

my %sortorder;
$sortorder{q} = 1;
$sortorder{r} = 2;
$sortorder{b} = 3;
$sortorder{n} = 4;
$sortorder{p} = 5;

# bishop is given a slightly higher value than knight here to ensure
# that kbkn is definitely preferred over knkb

my %values;
$values{q} = 9;
$values{r} = 5;
$values{b} = 3.1;
$values{n} = 3;
$values{p} = 1;

sub printnl {
    print XMLFILE @_, "\n";
}

my @normal_futurebases;
my @inverse_futurebases;

# Make an unordered pair of piece listings into a properly ordered
# filename (that might be color inverted, i.e, kkq to kqk).  Return
# the filename along with a flag indicating if we inverted.

sub mkfilename {
    my ($white_pieces, $black_pieces) = @_;

    $white_pieces = join('', sort { $sortorder{$a} <=> $sortorder{$b} } split(//, $white_pieces));
    $black_pieces = join('', sort { $sortorder{$a} <=> $sortorder{$b} } split(//, $black_pieces));

    my $white_value = 0;
    for my $white_piece (split(//, $white_pieces)) {
	$white_value += $values{$white_piece};
    }

    my $black_value = 0;
    for my $black_piece (split(//, $black_pieces)) {
	$black_value += $values{$black_piece};
    }

    if ((length($black_pieces) > length($white_pieces)) or
	((length($black_pieces) == length($white_pieces)) and ($black_value > $white_value))) {
	return (1, "k" . $black_pieces . "k" . $white_pieces);
    } else {
	return (0, "k" . $white_pieces . "k" . $black_pieces);
    }
}

sub mkfuturebase {
    my ($white_pieces, $black_pieces) = @_;

    my ($invert, $filename) = &mkfilename($white_pieces, $black_pieces);

    if ($invert) {
	if (grep($_ eq $filename, @inverse_futurebases) == 0) {
	    printnl '   <futurebase filename="' . $filename . '.htb" colors="invert"/>';
	    push @inverse_futurebases, $filename;
	}
    } else {
	if (grep($_ eq $filename, @normal_futurebases) == 0) {
	    printnl '   <futurebase filename="' . $filename . '.htb"/>';
	    push @normal_futurebases, $filename;
	}
    }
}

sub write_cntl_file {
    my ($cntl_filename) = @_;

    die "Invalid control filename $cntl_filename\n" unless ($cntl_filename =~ m/k([^k]*)k([^k.]*).xml/);
    my ($white_pieces, $black_pieces) = ($1, $2);

    @normal_futurebases = ();
    @inverse_futurebases = ();

    my ($invert, $filename) = &mkfilename($white_pieces, $black_pieces);

    return if $invert or $filename ne "k" . $white_pieces . "k" . $black_pieces;

    print "Writing $filename.xml\n";
    open (XMLFILE, ">$filename.xml");

    printnl '<?xml version="1.0"?>';
    printnl '<!DOCTYPE tablebase SYSTEM "http://www.freesoft.org/software/hoffman/tablebase.dtd">';
    printnl '<!-- Created by genctlfile.pl -->';
    printnl '';
    printnl '<tablebase>';

    printnl '   <dtm/>';
    printnl '   <piece color="white" type="king"/>';
    printnl '   <piece color="black" type="king"/>';
    for my $piece (split(//, $white_pieces)) {
	printnl '   <piece color="white" type="' . $pieces{$piece} . '"/>';
    }
    for my $piece (split(//, $black_pieces)) {
	printnl '   <piece color="black" type="' . $pieces{$piece} . '"/>';
    }

    for my $captured_white_index (1 .. length($white_pieces)) {
	my $remaining_white_pieces = $white_pieces;
	substr($remaining_white_pieces, $captured_white_index-1, 1) = "";
	&mkfuturebase($remaining_white_pieces, $black_pieces);
    }

    for my $captured_black_index (1 .. length($black_pieces)) {
	my $remaining_black_pieces = $black_pieces;
	substr($remaining_black_pieces, $captured_black_index-1, 1) = "";
	&mkfuturebase($white_pieces, $remaining_black_pieces);
    }

    if (index($white_pieces, 'p') != -1) {
	my $remaining_white_pieces = $white_pieces;
	substr($remaining_white_pieces, index($white_pieces, 'p'), 1) = "";
	for my $white_promotion (@non_pawn_pieces) {
	    &mkfuturebase($remaining_white_pieces . $white_promotion, $black_pieces);
	    for my $captured_black_index (1 .. length($black_pieces)) {
		if (substr($black_pieces, $captured_black_index-1, 1) ne "p") {
		    my $remaining_black_pieces = $black_pieces;
		    substr($remaining_black_pieces, $captured_black_index-1, 1) = "";
		    &mkfuturebase($remaining_white_pieces . $white_promotion, $remaining_black_pieces);
		}
	    }
	}
    }

    if (index($black_pieces, 'p') != -1) {
	my $remaining_black_pieces = $black_pieces;
	substr($remaining_black_pieces, index($black_pieces, 'p'), 1) = "";
	for my $black_promotion (@non_pawn_pieces) {
	    &mkfuturebase($white_pieces, $remaining_black_pieces . $black_promotion);
	    for my $captured_white_index (1 .. length($white_pieces)) {
		if (substr($white_pieces, $captured_white_index-1, 1) ne "p") {
		    my $remaining_white_pieces = $white_pieces;
		    substr($remaining_white_pieces, $captured_white_index-1, 1) = "";
		    &mkfuturebase($remaining_white_pieces, $remaining_black_pieces . $black_promotion);
		}
	    }
	}
    }

    printnl '   <output filename ="' . $filename . '.htb"/>';

    printnl '</tablebase>';
    close XMLFILE;

    return (@normal_futurebases, @inverse_futurebases);
}

sub all_combos_of_n_pieces {
    my ($n) = @_;

    if ($n == 0) {
	return ("");
    } elsif ($n == 1) {
	return @pieces;
    } else {
	my @result;

	for my $recursion (&all_combos_of_n_pieces($n - 1)) {
	    for my $piece (@pieces) {
		unshift @result, $recursion . $piece;
	    }
	}

	return @result;
    }
}

# Generate all tablebases with n white pieces and m black ones

sub gen {
    my ($white_n, $black_m) = @_;

    for my $white_pieces (&all_combos_of_n_pieces($white_n - 1)) {
	for my $black_pieces (&all_combos_of_n_pieces($black_m - 1)) {
	    &write_cntl_file($white_pieces, $black_pieces);
	}
    }
}


# Write all control files passed in as a list of filenames, recursing
# to write all dependencies as well.

sub write_cntl_files {
    my @cntl_files = @_;

    while ($#cntl_files >= 0) {
	my $cntl_filename = pop @cntl_files;
	if (-r $cntl_filename) {
	    # print "$cntl_filename exists, skipping\n";
	} else {
	    push @cntl_files, map { $_ . '.xml' } write_cntl_file($cntl_filename);
	}
    }
}

&write_cntl_files(@ARGV);
