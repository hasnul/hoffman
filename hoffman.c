/* -*- mode: C; eval: (c-set-style "stroustrup"); fill-column: 100 -*-
 *
 * HOFFMAN - a chess endgame tablebase builder
 *
 * by Brent Baccala
 *
 * August, 2006
 *
 * no rights reserved; you may freely copy, modify, or distribute HOFFMAN
 *
 * written in C for speed
 *
 * This program is formated for a (minimum) 100 character wide display.
 *
 * For those not up on Americana, the program is named after Trevor Hoffman, an All Star baseball
 * pitcher who specializes in "closing" games.  It was written specifically for The World vs. Arno
 * Nickel game.
 *
 * This program will calculate a tablebase for chess pieces (called the 'mobile' pieces) in a static
 * configuration of other 'frozen' pieces.  The mobile pieces could possibly be pawns.  The frozen
 * pieces could possibly be kings.
 *
 * Three piece tablebases with no frozen pieces can also be built.  These are the only tablebases
 * that are completely self contained and don't depend on other tablebases (the 'futurebases').
 *
 * Feed this program a list of futurebases on the command line.
 *
 * Feed this program an XML control file on the command line.
 */

#include <stdio.h>
/* #include <stdint.h> */
#include <stdlib.h>
#include <string.h>

typedef unsigned long long int int64;
typedef unsigned int int32;
typedef short boolean;


/***** GLOBAL CONSTANTS *****/

/* Maximum number of mobile pieces; used to simplify various arrays
 *
 * "8" may seem absurd, but it's probably about right.  "4" is easily doable in memory.  "5"
 * requires sweeping passes across a file on disk.  "6" and "7" are worse than "5", but doable with
 * severe restrictions on the movements of the pieces.  So "8" is enough.
 */

#define MAX_MOBILES 8

/* Why 100?  Well, I just think it's less likely to introduce bugs into this code if I count
 * half-moves instead of moves.  So it takes 100 half-moves to stalemate.
 */

#define STALEMATE_COUNT 100

/* seven possible pieces: KQRBNP, plus pawn that can be captured en passant 64 possible squares, up
 * to 8 directions per piece, up to 7 movements in one direction
 */

#define NUM_PIECES 7
#define NUM_SQUARES 64
#define NUM_DIR 8
#define NUM_MOVEMENTS 7


/***** DATA STRUCTURES *****/

/* position - the data structure that represents a board position
 *
 * How about if "position" is a structure containing an 8x8 char array with ASCII characters
 * representing each piece?  No.  Too slow.  Position manipulation is at the core of this program.
 * It has to be fast.
 *
 * So we use a 64-bit vector with one bit for each board position, in addition to a flag to indicate
 * which side is to move and four numbers (0-63) indicating the positions of the mobile pieces.
 * That way, we can easily check if possible moves are legal by looking for pieces that block our
 * moving piece.
 *
 * Also need a quick way to check captures.  Do this using a black_vector and a white_vector.
 *
 * We don't worry about moving a piece that's pinned on our king, for example.  The resulting
 * position will already have been flagged illegal in the table.
 *
 * We actually need to call this function a lot, so we want it to be fast, but I don't want to
 * optimize to the point where bugs can creep in.
 *
 * So how about a static 64-bit vector with bits set for the frozen pieces but not the mobiles?
 * Everytime we call index_to_position, copy from the static vector into the position structure.
 * Then we compute the positions of the mobile pieces and plug their bits into the structure's
 * vector at the right places.
 *
 */

/* Where are the kings located in the mobile piece list? */

#define WHITE_KING 0
#define BLACK_KING 1

typedef struct {
    int64 board_vector;
    int64 white_vector;
    int64 black_vector;
    short side_to_move;
    short mobile_piece_position[MAX_MOBILES];
} position;


/* bitvector get initialized in init_movements() */

int64 bitvector[64];
int64 allones_bitvector = 0xffffffffffffffffLL;

/* tablebase - the data structure used to hold tablebases
 *
 * WHITE and BLACK are also used for the side_to_move variable in the position type above
 */

#define KING 0
#define QUEEN 1
#define ROOK 2
#define BISHOP 3
#define KNIGHT 4
#define PAWN 5
#define PAWNep 6

char * piece_name[NUM_PIECES] = {"KING", "QUEEN", "ROOK", "BISHOP", "KNIGHT", "PAWN", "PAWNep"};

#define WHITE 0
#define BLACK 1


/**** TABLEBASE STRUCTURE AND OPERATIONS ****/

/* movecnt - 0 if this entry is ready to propagate; 255 if it has been propagated
 *
 * While movecnt is > 0, it is the number of moves FORWARD from this position that haven't been
 * analyzed yet.  The other three numbers are the number of moves out of this position for which
 * white wins, for which black wins, for which there is some kind of draw.
 *
 * If this position is WHITE TO MOVE, then we don't have to count outcomes which are WHITE WINS,
 * since that makes this position WHITE WINS.  We only have to count outcomes which are BLACK WINS,
 * in order to conclude that, if all possible white moves result in BLACK WINS, then this position
 * is BLACK WINS.  If at least one move leads to a draw (other moves lead to BLACK WINS), then the
 * position is WHITE DRAWS.  If all moves lead to draws, then the position is also BLACK DRAWS.
 * Since we assume that white will make the best move, then we can just say that this position DRAWS
 * unless either there is at least one move which leads to WHITE WINS, or if all moves lead to BLACK
 * WINS.
 *
 * So, all we really need is movecnt.  If we backtrace from a single WHITE WINS, then this position
 * becomes WHITE WINS.  If we backtrace from BLACK WINS, we decrement movecnt.  If movecnt reaches
 * zero, then the position becomes BLACK WINS.  When we're all done backtracing possible wins,
 * anything left with a non-zero movecnt is a DRAW.
 *
 * We also need a mate-in count and a stalemate (conversion) count.
 *
 * movecnt
 * 255 - ILLEGAL POSITION
 * 254 - WHITE WINS; propagation done
 * 253 - BLACK WINS; propagation done
 * 252 - WHITE WINS; propagation needed
 * 0   - BLACK WINS; propagation needed
 *
 * 1 through 251 - movecnt (during run), or DRAW (after run is finished)
 *
 * or, to make this work for either white or black positions, let's adopt the notation PTM (Player
 * to move) and PNTM (Player not to move)
 *
 * movecnt
 * 255 - ILLEGAL POSITION
 * 254 - PTM WINS; propagation done
 * 253 - PNTM WINS; propagation done
 * 252 - PTM WINS; propagation needed
 * 0   - PNTM WINS; propagation needed
 *
 * 1 through 251 - movecnt (during run), or DRAW (after run is finished)
 *
 */

#define ILLEGAL_POSITION 255
#define PTM_WINS_PROPAGATION_DONE 254
#define PNTM_WINS_PROPAGATION_DONE 253
#define PTM_WINS_PROPAGATION_NEEDED 252
#define PNTM_WINS_PROPAGATION_NEEDED 0

#define MAX_MOVECNT 251

struct fourbyte_entry {
    unsigned char movecnt;
    unsigned char mate_in_cnt;
    unsigned char stalemate_cnt;
    unsigned char reserved;
};

typedef struct {
    int num_mobiles;
    short mobile_piece_type[MAX_MOBILES];
    short mobile_piece_color[MAX_MOBILES];
    struct fourbyte_entry *entries;
} tablebase;

tablebase * parse_XML()
{
}

/* Simple initialization for a K+Q vs K endgame. */

tablebase * create_tablebase(void)
{
    tablebase *tb;

    tb = (tablebase *) malloc(sizeof(tablebase));
    if (tb == NULL) {
	fprintf(stderr, "Can't malloc tablebase header\n");
    }

    /* The "2" is because side-to-play is part of the position */

    tb->entries = (struct fourbyte_entry *) calloc(2*64*64*64, sizeof(struct fourbyte_entry));
    if (tb->entries == NULL) {
	fprintf(stderr, "Can't malloc tablebase entries\n");
    }

    tb->num_mobiles = 3;
    tb->mobile_piece_type[0] = KING;
    tb->mobile_piece_type[1] = KING;
    tb->mobile_piece_type[2] = QUEEN;
    tb->mobile_piece_color[0] = WHITE;
    tb->mobile_piece_color[1] = BLACK;
    tb->mobile_piece_color[2] = WHITE;

    return tb;
}

int32 max_index(tablebase *tb)
{
    return (2<<(6*tb->num_mobiles)) - 1;
}

int32 position_to_index(tablebase *tb, position *pos)
{
    /* This function, given a board position, returns an index into the tablebase.
     *
     * The reason we pass the tablebase in explicitly is that we will need to use this function to
     * calculate not only indices into our own table, but also into future tables with different
     * static configs.  Actually, I'm not sure about this.  Maybe it's only the matching function
     * index_to_position() that we need for future tables.  In any event, we'll need this function
     * to probe tables when we want to actually use them.
     *
     * Initially, this function can be very simple (multiplying numbers together), but to build
     * smaller tables it can be more precise.
     *
     * For example, two kings can never be next to each other.  Pieces can never be on top of each
     * other, or on top of static pieces.  The side to move can not be in check.
     *
     * Returns either an index into the table, or -1 (probably) if the position is illegal.
     *
     * Let's just ASSERT right now that this function can be used to check for illegal positions.
     * In fact, it is our primary function to check for illegal positions.  We call it and see if it
     * returns -1.
     */

    /* Keep it simple, for now */

    int shift_count = 1;
    int32 index = pos->side_to_move;  /* WHITE is 0; BLACK is 1 */
    int piece;

    for (piece = 0; piece < tb->num_mobiles; piece ++) {
	index |= pos->mobile_piece_position[piece] << shift_count;
	shift_count += 6;  /* because 2^6=64 */
    }

    return index;
}

/* OK, maybe not.  Maybe need to check index numbers, too. (Unless all positions in the table are
 * legal!)
 */

boolean check_legality_of_index(tablebase *config, int32 index)
{
}

/* any reason to do this?  just for one mobile? */
int index_to_mobile_position(tablebase *config, int32 index, int piece)
{}

boolean index_to_position(tablebase *tb, int32 index, position *p)
{
    /* Given an index, fill in a board position.  Obviously has to correspond to position_to_index()
     * and it's a big bug if it doesn't.  The boolean that gets returned is TRUE if the operation
     * succeeded (the index is at least minimally valid) and FALSE if the index is so blatantly
     * illegal (two piece on the same square) that we can't even fill in the position.
     */

    int piece;

    bzero(p, sizeof(position));

    p->side_to_move = index & 1;
    index >>= 1;

    for (piece = 0; piece < tb->num_mobiles; piece++) {
	p->mobile_piece_position[piece] = index & 63;
	if (p->board_vector & bitvector[index & 63]) {
	    return 0;
	}
	p->board_vector |= bitvector[index & 63];
	if (tb->mobile_piece_color[piece] == WHITE) {
	    p->white_vector |= bitvector[index & 63];
	} else {
	    p->black_vector |= bitvector[index & 63];
	}
	index >>= 6;
    }
    return 1;
}


/* "Designed to multi-thread"
 *
 * Keep atomic operations confined to single functions.  Design functions so that functions calling
 * them don't need to know the details of table format, either.
 *
 * These "add one" functions (atomically) add one to the count in question, subtract one from the
 * total move count, and flag the position as 'ready for propagation' (maybe this is just a move
 * count of zero) if the total move count goes to zero.
 */

#define WHITE_TO_MOVE(index) (((index)&1)==WHITE)
#define BLACK_TO_MOVE(index) (((index)&1)==BLACK)

inline short does_white_win(tablebase *tb, int32 index)
{
    if (WHITE_TO_MOVE(index)) {
	return (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE);
    } else {
	return (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE);
    }
}

inline short does_black_win(tablebase *tb, int32 index)
{
    if (BLACK_TO_MOVE(index)) {
	return (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE);
    } else {
	return (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE);
    }
}

inline boolean needs_propagation(tablebase *tb, int32 index)
{
    return (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
	|| (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED);
}

inline void mark_propagated(tablebase *tb, int32 index)
{
    if (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED) {
	tb->entries[index].movecnt = PTM_WINS_PROPAGATION_DONE;
    } else if (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED) {
	tb->entries[index].movecnt = PNTM_WINS_PROPAGATION_DONE;
    } else {
	fprintf(stderr, "Propagation attempt on a completed or unresolved position\n");
    }
}

/* get_mate_in_count() is also used as basically (does_white_win() || does_black_win()), so it has
 * to return -1 if there is no mate from this position
 */

inline int get_mate_in_count(tablebase *tb, int32 index)
{
    if (tb->entries[index].movecnt >= 1 && tb->entries[index].movecnt <= MAX_MOVECNT) {
	return -1;
    } else {
	return tb->entries[index].mate_in_cnt;
    }
}

inline int get_stalemate_count(tablebase *tb, int32 index)
{
    return tb->entries[index].stalemate_cnt;
}

inline void white_wins(tablebase *tb, int32 index, int mate_in_count, int stalemate_count)
{
    if (WHITE_TO_MOVE(index)) {
	if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		fprintf(stderr, "Mate in count dropped in white_wins!?\n");
	    }
	} else if ((tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
		   || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE)) {
	    fprintf(stderr, "White_wins in a position where black already won?!\n");
	} else {
	    tb->entries[index].movecnt = PTM_WINS_PROPAGATION_NEEDED;
	    tb->entries[index].mate_in_cnt = mate_in_count;
	    tb->entries[index].stalemate_cnt = stalemate_count;
	}
    } else {
	fprintf(stderr, "White_wins in a black to move position...\n");
	if ((tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE)) {
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		fprintf(stderr, "Mate in count dropped in white_wins!?\n");
	    }
	} else if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
		   || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    fprintf(stderr, "White_wins in a position where black already won?!\n");
	} else {
	    tb->entries[index].movecnt = PNTM_WINS_PROPAGATION_NEEDED;
	    tb->entries[index].mate_in_cnt = mate_in_count;
	    tb->entries[index].stalemate_cnt = stalemate_count;
	}
    }
}

inline void black_wins(tablebase *tb, int32 index, int mate_in_count, int stalemate_count)
{
    if (BLACK_TO_MOVE(index)) {
	if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		fprintf(stderr, "Mate in count dropped in black_wins!?\n");
	    }
	} else if ((tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
		   || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE)) {
	    fprintf(stderr, "Black_wins in a position where white already won?!\n");
	} else {
	    tb->entries[index].movecnt = PTM_WINS_PROPAGATION_NEEDED;
	    tb->entries[index].mate_in_cnt = mate_in_count;
	    tb->entries[index].stalemate_cnt = stalemate_count;
	}
    } else {
	fprintf(stderr, "Black_wins in a white to move position...\n");
	if ((tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_NEEDED)
	    || (tb->entries[index].movecnt == PNTM_WINS_PROPAGATION_DONE)) {
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		fprintf(stderr, "Mate in count dropped in black_wins!?\n");
	    }
	} else if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED)
		   || (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    fprintf(stderr, "Black_wins in a position where white already won?!\n");
	} else {
	    tb->entries[index].movecnt = PNTM_WINS_PROPAGATION_NEEDED;
	    tb->entries[index].mate_in_cnt = mate_in_count;
	    tb->entries[index].stalemate_cnt = stalemate_count;
	}
    }
}

inline void add_one_to_white_wins(tablebase *tb, int32 index, int mate_in_count, int stalemate_count)
{
    if (WHITE_TO_MOVE(index)) {
	fprintf(stderr, "add_one_to_white_wins in a white to move position\n");
    } else {
	if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED) ||
	    (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    /* This is OK.  Black is to move and already found a way to win.  Do nothing. */
	} else if ((tb->entries[index].movecnt == 0) || (tb->entries[index].movecnt > MAX_MOVECNT)) {
	    fprintf(stderr, "add_one_to_white_wins in an already won position!?\n");
	} else {
	    /* since PNTM_WIN_PROPAGATION_NEEDED is 0, this decrements right into the special flag,
	     * no extra check needed here
	     */
	    tb->entries[index].movecnt --;
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		if (tb->entries[index].mate_in_cnt != 255)
		    fprintf(stderr, "mate-in count dropped in add_one_to_white_wins?\n");
	    }
	    tb->entries[index].mate_in_cnt = mate_in_count;

	    /* XXX not sure about this stalemate code */
	    if (stalemate_count < tb->entries[index].stalemate_cnt) {
		tb->entries[index].stalemate_cnt = stalemate_count;
	    }
	}
    }
}

inline void add_one_to_black_wins(tablebase *tb, int32 index, int mate_in_count, int stalemate_count)
{
    if (BLACK_TO_MOVE(index)) {
	fprintf(stderr, "add_one_to_black_wins in a black to move position\n");
    } else {
	if ((tb->entries[index].movecnt == PTM_WINS_PROPAGATION_NEEDED) ||
	    (tb->entries[index].movecnt == PTM_WINS_PROPAGATION_DONE)) {
	    /* This is OK.  White is to move and already found a way to win.  Do nothing. */
	} else if ((tb->entries[index].movecnt == 0) || (tb->entries[index].movecnt > MAX_MOVECNT)) {
	    fprintf(stderr, "add_one_to_black_wins in an already won position!?\n");
	} else {
	    /* since PNTM_WIN_PROPAGATION_NEEDED is 0, this decrements right into the special flag,
	     * no extra check needed here
	     */
	    tb->entries[index].movecnt --;
	    if (mate_in_count < tb->entries[index].mate_in_cnt) {
		if (tb->entries[index].mate_in_cnt != 255)
		    fprintf(stderr, "mate-in count dropped in add_one_to_black_wins?\n");
	    }
	    tb->entries[index].mate_in_cnt = mate_in_count;

	    /* XXX not sure about this stalemate code */
	    if (stalemate_count < tb->entries[index].stalemate_cnt) {
		tb->entries[index].stalemate_cnt = stalemate_count;
	    }
	}
    }
}

/* Five possible ways we can initialize an index for a position:
 *  - it's illegal
 *  - white's mated (black is to move)
 *  - black's mated (white is to move)
 *  - stalemate
 *  - any other position, with 'movecnt' possible moves out the position
 */

void initialize_index_as_illegal(tablebase *tb, int32 index)
{
    tb->entries[index].movecnt = ILLEGAL_POSITION;
    tb->entries[index].mate_in_cnt = 255;
    tb->entries[index].stalemate_cnt = 255;
}

void initialize_index_with_white_mated(tablebase *tb, int32 index)
{
    if (WHITE_TO_MOVE(index)) {
	fprintf(stderr, "initialize_index_with_white_mated in a white to move position!\n");
    }
    tb->entries[index].movecnt = PTM_WINS_PROPAGATION_NEEDED;
    tb->entries[index].mate_in_cnt = 0;
    tb->entries[index].stalemate_cnt = 0;
    black_wins(tb, index, 0, 0);
}

void initialize_index_with_black_mated(tablebase *tb, int32 index)
{
    if (BLACK_TO_MOVE(index)) {
	fprintf(stderr, "initialize_index_with_black_mated in a black to move position!\n");
    }
    tb->entries[index].movecnt = PTM_WINS_PROPAGATION_NEEDED;
    tb->entries[index].mate_in_cnt = 0;
    tb->entries[index].stalemate_cnt = 0;
}

void initialize_index_with_stalemate(tablebase *tb, int32 index)
{
    tb->entries[index].movecnt = 251; /* use this as stalemate for now */
    tb->entries[index].mate_in_cnt = 255;
    tb->entries[index].stalemate_cnt = 0;
}

void initialize_index_with_movecnt(tablebase *tb, int32 index, int movecnt)
{
    tb->entries[index].movecnt = movecnt;
    tb->entries[index].mate_in_cnt = 255;
    tb->entries[index].stalemate_cnt = 255;
}


/***** MOVEMENT VECTORS *****/

/* The idea here is to calculate piece movements, and to do it FAST.
 *
 * We build a table of "movements" organized into "directions".  Each direction is just that - the
 * direction that a piece (like a queen) moves.  When we want to check for what movements are
 * possible in a given direction, we run through the direction until we "hit" another pieces - until
 * the bit in the vector matches something already in the position vector.  At the end of the
 * direction, an all-ones vector will "hit" the end of the board and end the direction.  I know,
 * kinda confusing.  It's because it's designed to be fast; we have to do this a lot.
 */

struct movement {
    int64 vector;
    short square;
};

/* we add one to NUM_MOVEMENTS to leave space at the end for the all-ones bitmask that signals the
 * end of the list
 */

struct movement movements[NUM_PIECES][NUM_SQUARES][NUM_DIR][NUM_MOVEMENTS+1];

/* How many different directions can each piece move in?  Knights have 8 directions because they
 * can't be blocked in any of them.
 */

int number_of_movement_directions[7] = {8,8,4,4,8,1,1};
int maximum_movements_in_one_direction[7] = {1,7,7,7,1,2,1};

enum {RIGHT, LEFT, UP, DOWN, DIAG_UL, DIAG_UR, DIAG_DL, DIAG_DR, KNIGHTmove, PAWNmove, PAWN2move}
movementdir[7][8] = {
    {RIGHT, LEFT, UP, DOWN, DIAG_UL, DIAG_UR, DIAG_DL, DIAG_DR},	/* King */
    {RIGHT, LEFT, UP, DOWN, DIAG_UL, DIAG_UR, DIAG_DL, DIAG_DR},	/* Queen */
    {RIGHT, LEFT, UP, DOWN},						/* Rook */
    {DIAG_UL, DIAG_UR, DIAG_DL, DIAG_DR},				/* Bishop */
    {KNIGHTmove, KNIGHTmove, KNIGHTmove, KNIGHTmove, KNIGHTmove, KNIGHTmove, KNIGHTmove, KNIGHTmove},	/* Knights are special... */
    {PAWNmove, PAWN2move},						/* Pawns need more work */
    {PAWNmove},								/* en passant pawns */
};



void init_movements()
{
    int square, piece, dir, mvmt;

    for (square=0; square < NUM_SQUARES; square++) {
	bitvector[square] = 1ULL << square;
    }

    for (piece=0; piece < NUM_PIECES; piece++) {
	for (square=0; square < NUM_SQUARES; square++) {

	    for (dir=0; dir < number_of_movement_directions[piece]; dir++) {

		int current_square = square;

		for (mvmt=0; mvmt < maximum_movements_in_one_direction[piece]; mvmt ++) {

#define RIGHT_MOVEMENT_POSSIBLE ((current_square%8)<7)
#define RIGHT2_MOVEMENT_POSSIBLE ((current_square%8)<6)
#define LEFT_MOVEMENT_POSSIBLE ((current_square%8)>0)
#define LEFT2_MOVEMENT_POSSIBLE ((current_square%8)>1)
#define UP_MOVEMENT_POSSIBLE (current_square<56)
#define UP2_MOVEMENT_POSSIBLE (current_square<48)
#define DOWN_MOVEMENT_POSSIBLE (current_square>7)
#define DOWN2_MOVEMENT_POSSIBLE (current_square>15)

		    switch (movementdir[piece][dir]) {
		    case RIGHT:
			if (RIGHT_MOVEMENT_POSSIBLE) {
			    current_square++;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case LEFT:
			if (LEFT_MOVEMENT_POSSIBLE) {
			    current_square--;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case UP:
			if (UP_MOVEMENT_POSSIBLE) {
			    current_square+=8;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case DOWN:
			if (DOWN_MOVEMENT_POSSIBLE) {
			    current_square-=8;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case DIAG_UL:
			if (LEFT_MOVEMENT_POSSIBLE && UP_MOVEMENT_POSSIBLE) {
			    current_square+=8;
			    current_square--;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case DIAG_UR:
			if (RIGHT_MOVEMENT_POSSIBLE && UP_MOVEMENT_POSSIBLE) {
			    current_square+=8;
			    current_square++;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case DIAG_DL:
			if (LEFT_MOVEMENT_POSSIBLE && DOWN_MOVEMENT_POSSIBLE) {
			    current_square-=8;
			    current_square--;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case DIAG_DR:
			if (RIGHT_MOVEMENT_POSSIBLE && DOWN_MOVEMENT_POSSIBLE) {
			    current_square-=8;
			    current_square++;
			    movements[piece][square][dir][mvmt].square = current_square;
			    movements[piece][square][dir][mvmt].vector = bitvector[current_square];
			} else {
			    movements[piece][square][dir][mvmt].square = -1;
			    movements[piece][square][dir][mvmt].vector = allones_bitvector;
			}
			break;
		    case KNIGHTmove:
			current_square=square;
			switch (dir) {
			case 0:
			    if (RIGHT2_MOVEMENT_POSSIBLE && UP_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square + 2 + 8;
				movements[piece][square][dir][0].vector = bitvector[square + 2 + 8];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 1:
			    if (RIGHT2_MOVEMENT_POSSIBLE && DOWN_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square + 2 - 8;
				movements[piece][square][dir][0].vector = bitvector[square + 2 - 8];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 2:
			    if (LEFT2_MOVEMENT_POSSIBLE && UP_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square - 2 + 8;
				movements[piece][square][dir][0].vector = bitvector[square - 2 + 8];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 3:
			    if (LEFT2_MOVEMENT_POSSIBLE && DOWN_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square - 2 - 8;
				movements[piece][square][dir][0].vector = bitvector[square - 2 - 8];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 4:
			    if (RIGHT_MOVEMENT_POSSIBLE && UP2_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square + 1 + 16;
				movements[piece][square][dir][0].vector = bitvector[square + 1 + 16];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 5:
			    if (RIGHT_MOVEMENT_POSSIBLE && DOWN2_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square + 1 - 16;
				movements[piece][square][dir][0].vector = bitvector[square + 1 - 16];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 6:
			    if (LEFT_MOVEMENT_POSSIBLE && UP2_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square - 1 + 16;
				movements[piece][square][dir][0].vector = bitvector[square - 1 + 16];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			case 7:
			    if (LEFT_MOVEMENT_POSSIBLE && DOWN2_MOVEMENT_POSSIBLE) {
				movements[piece][square][dir][0].square = square - 1 - 16;
				movements[piece][square][dir][0].vector = bitvector[square - 1 - 16];
				movements[piece][square][dir][1].square = -1;
				movements[piece][square][dir][1].vector = allones_bitvector;
			    } else {
				movements[piece][square][dir][0].square = -1;
				movements[piece][square][dir][0].vector = allones_bitvector;
			    }
			    break;
			}
			break;

		    case PAWNmove:
		    case PAWN2move:
			/* Oh, we need to distinguish between forward/backward here as well as white
			 * and black pawns...
			 */
			break;
		    }
		}

		/* Always put an allones_bitvector at the end of the movement vector
		 * to make sure we stop!
		 */

		movements[piece][square][dir][mvmt].square = -1;
		movements[piece][square][dir][mvmt].vector = allones_bitvector;

	    }
	}
    }
}

/* I don't plan to call this routine every time the program runs, but it has to be used after any
 * changes to the code above to verify that those complex movement vectors are correct, or at least
 * consistent.  We're using this in a game situation.  We can't afford bugs in this code.
 */

void verify_movements()
{
    int piece;
    int squareA, squareB;
    int dir;
    struct movement * movementptr;

    /* For everything except pawns, if it can move from A to B, then it better be able to move from
     * B to A...
     */

    for (piece=KING; piece <= KNIGHT; piece ++) {

	for (squareA=0; squareA < NUM_SQUARES; squareA ++) {

	    for (squareB=0; squareB < NUM_SQUARES; squareB ++) {

		int movement_possible = 0;
		int reverse_movement_possible = 0;

		if (squareA == squareB) {
		    for (dir = 0; dir < number_of_movement_directions[piece]; dir++) {
			for (movementptr = movements[piece][squareA][dir];
			     (movementptr->vector & bitvector[squareB]) == 0;
			     movementptr++) ;
			if ((movementptr->square != -1) || (movementptr->vector != allones_bitvector)) {
			    fprintf(stderr, "Self movement possible!? %s %d %d\n",
				    piece_name[piece], squareA, movementptr->square);
			}
		    }
		    continue;
		}

		for (dir = 0; dir < number_of_movement_directions[piece]; dir++) {

		    for (movementptr = movements[piece][squareA][dir];
			 (movementptr->vector & bitvector[squareB]) == 0;
			 movementptr++) {
			if ((movementptr->square < 0) || (movementptr->square >= NUM_SQUARES)) {
			    fprintf(stderr, "Bad movement square: %s %d %d %d\n",
				    piece_name[piece], squareA, squareB, movementptr->square);
			}
		    }

		    if (movementptr->square == -1) {
			if (movementptr->vector != allones_bitvector) {
			    fprintf(stderr, "-1 movement lacks allones_bitvector: %s %d %d\n",
				    piece_name[piece], squareA, squareB);
			}
		    } else if ((movementptr->square < 0) || (movementptr->square >= NUM_SQUARES)) {
			fprintf(stderr, "Bad movement square: %s %d %d\n",
				piece_name[piece], squareA, squareB);
		    } else {
			if (movementptr->square != squareB) {
			    fprintf(stderr, "bitvector does not match destination square: %s %d %d\n",
				    piece_name[piece], squareA, squareB);
			}
			if (movement_possible) {
			    fprintf(stderr, "multiple idential destinations from same origin: %s %d %d\n",
				    piece_name[piece], squareA, squareB);
			}
			movement_possible = 1;
			if (movementptr->vector == allones_bitvector) {
			    fprintf(stderr, "allones_bitvector on a legal movement: %s %d %d\n",
				    piece_name[piece], squareA, squareB);
			}
		    }
		}


		for (dir = 0; dir < number_of_movement_directions[piece]; dir++) {

		    for (movementptr = movements[piece][squareB][dir];
			 (movementptr->vector & bitvector[squareA]) == 0;
			 movementptr++) ;

		    if (movementptr->square != -1) reverse_movement_possible=1;
		}


		if (movement_possible && !reverse_movement_possible) {
		    fprintf(stderr, "reverse movement impossible: %s %d %d\n",
			    piece_name[piece], squareA, squareB);
		}

	    }
	}
    }
}


/***** FUTUREBASES *****/

#ifdef FUTUREBASES

calculate_all_possible_futuremoves()
{
    consider all possible captures;

    consider all possible pawn moves, including queening and knighting;

    put them in some kind of list or array;

    flag them according to our pruning instructions;

    sort them so that if the same table is used for several positions, they appear together on the list;
								
    later: strike them off the list as we process their respective files;
}

propagate_position_from_future_table(position)
{
    for (all possible captures and pawn moves in position) {

	check if one of our future tables matches this move;

	if so {
	    fetch result from future table;
	    if (result == white_wins) break;
	    if (result == white_draws) {
		result = white_draws;
		continue;
		/* keep looking for a win */
	    }
	    /* but whose move is it? */
	}

	else if move is flagged prune-our-move, decrement counts;

	else if move is flagged prune-his-move {

	    for (all possible responses to his move) {
		if (one of our future tables matches a white win) {
		    propagate a white win;
		}
		elsif (one of our future tables matches a white draw) {
		    propagate a white draw;
		} else {
		    /* this is where we vary for a more complex program */
		    propagate a black win;
		}
	    }
	}
    }

    /* This is where we make pruning decisions, if we don't want to fully analyze out the tree past
     * the table we're now building.  Of course, this will affect the accuracy of the table; the
     * table is a result of BOTH the position it was set up for AND the pruning decisions (and any
     * pruning decisions made on the future tables used to calculate this one).
     *
     * We specify pruning in a simple way - by omitting future tables for moves we don't want to
     * consider.  This can be dangerous, so we require this feature to be specifically enabled for
     * each move by a command-line switch.  Actually, we use two switches, one to calculate a table
     * for OUR SIDE to move, and another if it is the OTHER SIDE to move.
     *
     * So, --prune-our-move e3e4 prunes a pawn move (assuming this is a table with a static pawn on
     * e3) by simply ignoring e3e4 as a possible move.
     *
     * Pruning an opponent's move is more complex because we step a half-move into the future and
     * consider our own next move.  This costs us little, since we can control our own move and
     * therefore don't have to consider all possibilities, and improves a lot.  If future tables
     * exist for any of our responses, they are used.  If no such future tables exist, then the move
     * is regarded as a lost game.
     *
     * So, --prune-his-move e7e8 prunes a pawn promotion (assuming a static pawn on e7) by
     * considering all possible positions resulting after the pawn promotion (to either Q or N) AND
     * the answering move.  The resulting game is regarded as a win for white unless both Q and N
     * promotions have an answer that leads to another table with a win or draw for black.
     *
     * For example, let's say we're looking at a Q-and-P vs. Q-and-P endgame.  There are four mobile
     * pieces (2 Ks and 2 Qs), so we can handle this.  But if one of the pawns queens, then we've
     * got a game with five mobile pieces, and that's too complex.  But we don't want to completely
     * discard all possible enemy promotions, if we can immediately capture the new queen (or the
     * old one).  So we specify something like --prune-his-move e7e8 and pass in a tablebase for a
     * Q-and-P vs. Q endgame.
     *
     * We also check for immediate checkmates or stalemates.
     *
     * Question: do we really need to flag this at all?  Probably yes, because we don't want this
     * pruning to occur by accident.
     *
     * Another reason to flag it is that we want to label in the file header that this pruning was
     * done.  In particular, if we use a pruned tablebase to compute another (earlier) pruned
     * tablebase, we want to make sure the pruning is consistent, i.e. "our" side has to stay the
     * same.  This can only be guaranteed if we explicitly flag which side is which in the file
     * header.
     *
     * Pruning doesn't affect the size of the resulting tablebase.  We discard the extra
     * information.  If the pruned move is actually made in the game, then you have to calculate all
     * possible next moves and check your tablebases for them.  This seems reasonable.
     *
     */

}

propagate_move_from_future_table()
{
    if (future_table resulted from capture) {
	/* need to consider pawn captures seperately? */

    } else if (future_table resulted from pawn move) {

	future_table could result from pawn queening;
	future_table could result from pawn knighting;

    }
}

propagate_moves_from_futurebases()
{
    for (all legal positions in our table) {
	propagate_position_from_future_table(position);
    }
}

propagate_moves_from_futurebases()
{
    calculate_all_possible_futuremoves();

    for (all futurebases on command line or control file) {
	propagate_moves_from_futurebase();
    }

    if (any futuremoves still unhandled) {
	die(error);
    }
}

#endif

/* Intra-table move propagation.
 *
 * This is the guts of the program here.  We've got a move that needs to be propagated,
 * so we back out one half-moves to all of the positions that could have gotten us
 * here and update their counters in various obscure ways.
 */

void propagate_move_within_table(tablebase *tb, int32 parent_index, int mate_in_count)
{
    position parent_position;
    position current_position; /* i.e, last position that moved to parent_position */
    int piece;
    int dir;
    struct movement *movementptr;
    int32 current_index;

    /* ASSERT (table,index) == WIN/LOSS IN x MOVES or DRAW; */
    if (get_mate_in_count(tb, parent_index) != mate_in_count) {
	fprintf(stderr, "Mate-in counts don't match: %d %d\n",
		mate_in_count, get_mate_in_count(tb, parent_index));
    }

    if (!does_white_win(tb, parent_index) && !does_black_win(tb, parent_index)) {
	fprintf(stderr, "Propagating position %d where neither side wins?!\n", parent_index);
    }

    /* We want to check to make sure the mate-in number of the position in the database matches a
     * mate-in variable in this routine.  If we're propagating moves from a future table, we might
     * get tables with a whole range of mate-in counts, so we want to make sure we go through them
     * in order.
     */

    mark_propagated(tb, parent_index);

    index_to_position(tb, parent_index, &parent_position);

    /* foreach (mobile piece of player NOT TO PLAY) { */

    for (piece = 0; piece < tb->num_mobiles; piece++) {

	/* We've moving BACKWARDS in the game, so we want the pieces of the player who is NOT TO
	 * PLAY here - this is the LAST move we're considering, not the next move.
	 */

	if (tb->mobile_piece_color[piece] == parent_position.side_to_move)
	    continue;

	/* current_position[piece] is a number from 0 to 63 corresponding to the different squares
	 * on the chess board
	 */

	/* possible_moves returns a pointer to an array of possible new position numbers for this
	 * piece.  These are BACKWARDS moves in the game.  The positions returned are legal.
	 *
	 * possible_moves has to look at the entire current position to build this array, because
	 * otherwise it might move a piece "through" another piece.
	 */

	/* possible_moves(current_position[piece], type_of[piece]); */

	for (dir = 0; dir < number_of_movement_directions[tb->mobile_piece_type[piece]]; dir++) {

	    /* What about captures?  Well, first of all, there are no captures here!  We're moving
	     * BACKWARDS in the game... and pieces don't appear out of thin air.  Captures are
	     * handled by back-propagation from futurebases, not here in the movement code.  The
	     * piece moving had to come from somewhere, and that somewhere will now be an empty
	     * square, so once we've hit another piece along a movement vector, there's absolutely
	     * no need to consider anything further.
	     */

	    for (movementptr
		     = movements[tb->mobile_piece_type[piece]][parent_position.mobile_piece_position[piece]][dir];
		 (movementptr->vector & parent_position.board_vector) == 0;
		 movementptr++) {

		current_position = parent_position;

		if (parent_position.side_to_move == WHITE)
		    current_position.side_to_move = BLACK;
		else
		    current_position.side_to_move = WHITE;

		/* This code makes perfect sense... but I doubt it will be needed!  The
		 * position_to_index function will probably only require the square numbers, not the
		 * board vectors.
		 */
#if NEEDED
		current_position.board_vector &= ~bitvector[parent_position.mobile_piece_position[piece]];
		current_position.board_vector |= bitvector[movementptr->square];
		if (tb->mobile_piece_color[piece] == WHITE) {
		    current_position.white_vector &= ~bitvector[parent_position.mobile_piece_position[piece]];
		    current_position.white_vector |= bitvector[movementptr->square];
		} else {
		    current_position.black_vector &= ~bitvector[parent_position.mobile_piece_position[piece]];
		    current_position.black_vector |= bitvector[movementptr->square];
		}
#endif

		current_position.mobile_piece_position[piece] = movementptr->square;

		current_index = position_to_index(tb, &current_position);

		/* Parent position is the FUTURE position */

		/* all of these subroutines have to propagate if changed */

		/* these stalemate and mate counts increment by one every HALF MOVE */

		if (parent_position.side_to_move == WHITE) {

		    /* ...then this position is BLACK TO MOVE */

		    if (does_white_win(tb, parent_index)) {

			/* parent position is WHITE MOVES AND WINS */
			if (get_stalemate_count(tb, parent_index) < STALEMATE_COUNT) {
			    add_one_to_white_wins(tb, current_index,
						  get_mate_in_count(tb, parent_index)+1,
						  get_stalemate_count(tb, parent_index)+1);
			}

		    } else if (does_black_win(tb, parent_index)) {

			/* parent position is WHITE MOVES AND BLACK WINS */
			if (get_stalemate_count(tb, parent_index) < STALEMATE_COUNT) {
			    black_wins(tb, current_index,
				       get_mate_in_count(tb, parent_index)+1,
				       get_stalemate_count(tb, parent_index)+1);
			}

		    }

		} else {

		    /* or this position is WHITE TO MOVE */

		    if (does_black_win(tb, parent_index)) {

			/* parent position is BLACK MOVES AND WINS */
			if (get_stalemate_count(tb, parent_index) < STALEMATE_COUNT) {
			    add_one_to_black_wins(tb, current_index,
						  get_mate_in_count(tb, parent_index)+1,
						  get_stalemate_count(tb, parent_index)+1);
			}

		    } else if (does_white_win(tb, parent_index)) {

		        /* parent position is BLACK MOVES AND WHITE WINS */
			if (get_stalemate_count(tb, parent_index) < STALEMATE_COUNT) {
			    white_wins(tb, current_index,
				       get_mate_in_count(tb, parent_index)+1,
				       get_stalemate_count(tb, parent_index)+1);
			}

		    }
		}
	    }
	}

    }
}

initialize_tablebase(tablebase *tb)
{
    position parent_position;
    position current_position;
    int32 index;
    int piece;
    int dir;
    struct movement *movementptr;

    /* This is here because we don't want to be calling max_index() everytime through the loop below */

    int32 max_index_static = max_index(tb);

    for (index=0; index < max_index_static; index++) {

	if (! index_to_position(tb, index, &parent_position)) {

	    initialize_index_as_illegal(tb, index);

	} else {

	    /* Now we need to count moves.  FORWARD moves. */
	    int movecnt = 0;

	    for (piece = 0; piece < tb->num_mobiles; piece++) {

		/* We only want to consider pieces of the side which is to move... */

		if (tb->mobile_piece_color[piece] != parent_position.side_to_move)
		    continue;

		for (dir = 0; dir < number_of_movement_directions[tb->mobile_piece_type[piece]]; dir++) {

		    current_position = parent_position;

		    for (movementptr = movements[tb->mobile_piece_type[piece]][parent_position.mobile_piece_position[piece]][dir];
			 (movementptr->vector & current_position.board_vector) == 0;
			 movementptr++) {

			movecnt ++;
		    }

		    /* Now check to see if the movement ended because we hit against another piece
		     * of the opposite color.  If so, add another move for the capture.
		     *
		     * Actually, we check to see that we DIDN'T hit a piece of our OWN color.  The
		     * difference is that this way we don't register a capture if we hit the end of
		     * the list of movements in a given direction.
		     *
		     * We also check to see if the capture was against the enemy king! in which case
		     * this position is a "mate in 0" (i.e, illegal)
		     *
		     * XXX ASSUMES THAT THE ENEMY KING IS ONE OF THE MOBILE PIECES XXX
		     */

		    if (current_position.side_to_move == WHITE) {
			if ((movementptr->vector & current_position.white_vector) == 0) {
			    movecnt ++;
			    if (movementptr->square ==
				current_position.mobile_piece_position[BLACK_KING]) {
				initialize_index_with_black_mated(tb, index);
				goto mated;
			    }
			}
		    } else {
			if ((movementptr->vector & current_position.black_vector) == 0) {
			    movecnt ++;
			    if (movementptr->square ==
				current_position.mobile_piece_position[WHITE_KING]) {
				initialize_index_with_white_mated(tb, index);
				goto mated;
			    }
			}
		    }

		}

#ifdef FROZEN_PIECES
		for (;;) {
		    forall (possible moves of frozen pieces) movecnt++;
		}
#endif
	    }

	    if (movecnt == 0) initialize_index_with_stalemate(tb, index);
	    else initialize_index_with_movecnt(tb, index, movecnt);

	mated: ;
				
	}
    }
}

main()
{
    tablebase *tb;
    int max_moves_to_win;
    int moves_to_win;
    int progress_made;
    int32 max_index_static;
    int32 index;

    /* create_data_structure_from_control_file(); */

    init_movements();

    verify_movements();

    /* exit(1); */

    tb = create_tablebase();

    initialize_tablebase(tb);

#ifdef FUTUREBASE
    max_moves_to_win = propagate_moves_from_futurebases();
#else
    max_moves_to_win = 1;
#endif

    /* First we look for forced mates... */

    moves_to_win = 0;
    progress_made = 1;
    max_index_static = max_index(tb);

    while (progress_made || moves_to_win < max_moves_to_win) {
	progress_made = 0;
	for (index=0; index < max_index_static; index++) {
	    if (needs_propagation(tb, index) && get_mate_in_count(tb, index) == moves_to_win) {
		if (!progress_made)
		    fprintf(stderr, "Pass %d starts with %d\n", moves_to_win, index);
		propagate_move_within_table(tb, index, moves_to_win);
		progress_made ++;
	    }
	}
	fprintf(stderr, "Pass %d complete; %d positions processed\n", moves_to_win, progress_made);
	moves_to_win ++;
    }

    /* ...then we look for forced draws */

#if 0
    progress_made = 1;

    while (progress_made) {
	for (index=0; index < max_index_static; index++) {
	    if (needs_propagation(tb, index)
		&& (does_white_draw(tb, index) || does_black_draw(tb, index))) {
		propagate_move_within_table(tb, index);
		progress_made = 1;
	    }
	}
    }
#endif

    /* Everything else allows both sides to draw with best play.
     *
     * Perhaps this seems a bit strange.  After all, if white can force a draw but not a win, then
     * can't black force a draw, too?  So what's the difference between the forced draws we
     * calculated above and a draw by repetitions?  You have to keep movement restrictions in mind.
     * If your pieces are restricted in how they can move, then the computer might only be able to
     * tell you that you can force a draw, even though you might be able to force a win.
     *
     * Actually, even this doesn't make complete sense.  Maybe we don't need that forced
     * draw code at all.  Maybe this is all we need.
     */

    /* flag_everything_else_drawn_by_repetition(); */

    /* write_output_tablebase(); */
}
