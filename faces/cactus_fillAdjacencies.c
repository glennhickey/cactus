#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <getopt.h>

#include "cactus.h"
#include "commonC.h"
#include "fastCMaths.h"
#include "bioioC.h"
#include "hashTableC.h"
#include "cactus_buildFaces.h"

/*
 * Globals definitions
 */
typedef struct adjacency_vote_st AdjacencyVote;
typedef struct adjacency_vote_table_st AdjacencyVoteTable;

/*
 * Global variables
 */
static int VOTE_CUTOFF = 16;

///////////////////////////////////////////////
// Adjacency vote :
//      message passing structure for ambiguous
//      partner configurations
///////////////////////////////////////////////
struct adjacency_vote_st {
	int length;
	bool intersection;
	Cap **candidates;
};

/*
 * Basic empty constructor
 */
static AdjacencyVote *adjacencyVote_construct(int32_t length)
{
	AdjacencyVote *vote = st_calloc(1, sizeof(AdjacencyVote));

	vote->intersection = true;
	vote->length = length;
	vote->candidates = st_calloc(length, sizeof(Cap *));

	return vote;
}

/*
 * Destructor
 */
static void adjacencyVote_destruct(AdjacencyVote * vote)
{
	free(vote);
}

/*
 * Copy function
 */
static AdjacencyVote *adjacencyVote_copy(AdjacencyVote * vote)
{
	AdjacencyVote *copy = adjacencyVote_construct(vote->length);
	int index;

	for (index = 0; index < vote->length; index++)
		copy->candidates[index] = vote->candidates[index];

	return copy;
}

static AdjacencyVote *adjacencyVoteTable_getVote(Cap * cap,
						 AdjacencyVoteTable *
						 table);
/*
 * Searches a Cap's ancestors in search of given node
 */
static int32_t adjacencyVote_isDescendantOf(Cap * descendant,
					    Cap * ancestor,
					    AdjacencyVoteTable * table)
{
	Cap *current = descendant;

	if (current == ancestor)
		return true;

	if (cap_getParent(current) == ancestor)
		return true;

	while ((current = cap_getParent(current))
	       && adjacencyVoteTable_getVote(current, table)) {
		st_logInfo("Testing %p\n", current);
		if (cap_getParent(current) == ancestor)
			return true;
	}

	return false;
}

/*
 * Search for particular value
 */
static Cap *adjacencyVote_containsDescent(AdjacencyVote * vote,
						  Cap *
						  cap,
						  AdjacencyVoteTable *
						  table)
{
	int index;
	End *end = cap_getEnd(cap);

	for (index = 0; index < vote->length; index++) {
		if (cap_getEnd(vote->candidates[index]) != end)
			continue;
		else if (adjacencyVote_isDescendantOf
			 (vote->candidates[index], cap, table))
			return vote->candidates[index];
	}

	return NULL;
}

/*
 * Return number of candidates
 */
static int adjacencyVote_getLength(AdjacencyVote * vote)
{
	return vote->length;
}

/*
 * Return first value
 */
static Cap *adjacencyVote_getWinner(AdjacencyVote * vote)
{
	return vote->candidates[0];
}

/*
 * Checks if vote points to NULL
 */
static bool adjacencyVote_isBlank(AdjacencyVote * vote) {
	return vote->length == 1 && vote->candidates[0] == NULL;
}

/*
 * Determine what adjacencies are feasible for a given node depending on the adjacencies of children
 */
static AdjacencyVote *adjacencyVote_processVotes(AdjacencyVote * vote_1,
						 AdjacencyVote * vote_2)
{
	AdjacencyVote *merged_vote;
	int index_merged = 0;
	int index_1 = 0;
	int index_2 = 0;
	bool blank_1 = adjacencyVote_isBlank(vote_1);
	bool blank_2 = adjacencyVote_isBlank(vote_2);

	if (blank_1 && blank_2)
		return adjacencyVote_copy(vote_1);
	else if (blank_1)
		return adjacencyVote_copy(vote_2);
	else if (blank_2)
		return adjacencyVote_copy(vote_1);

	merged_vote = adjacencyVote_construct(vote_1->length + vote_2->length);

	// Computing intersection first
	while (index_1 < vote_1->length && index_2 < vote_2->length) {
		if (vote_1->candidates[index_1] == vote_2->candidates[index_2]) {
			merged_vote->candidates[index_merged++] =
			    vote_1->candidates[index_1++];
			index_2++;
		}
		else if (cap_getName(vote_1->candidates[index_1]) <
		         cap_getName(vote_2->candidates[index_2]))
			index_1++;
		else if (cap_getName(vote_1->candidates[index_1]) >
			 cap_getName(vote_2->candidates[index_2]))
			index_2++;
	}

	if (index_merged > 0) {
		merged_vote->length = index_merged;
		return merged_vote;
	}

	index_merged = 0;
	index_1 = 0;
	index_2 = 0;
	merged_vote->intersection = false;

	// If intersection failed: union
	while (index_1 < vote_1->length || index_2 < vote_2->length) {
		if (index_1 < vote_1->length
		    && index_2 < vote_2->length
		    && vote_1->candidates[index_1] == vote_2->candidates[index_2]) {
			merged_vote->candidates[index_merged++] =
			    vote_1->candidates[index_1++];
			index_2++;
		}
		else if (index_2 == vote_2->length)
			merged_vote->candidates[index_merged++] =
			    vote_1->candidates[index_1++];
		else if (index_1 == vote_1->length)
			merged_vote->candidates[index_merged++] =
			    vote_2->candidates[index_2++];
		else if (cap_getName(vote_1->candidates[index_1]) <
		         cap_getName(vote_2->candidates[index_2]))
			merged_vote->candidates[index_merged++] =
			    vote_1->candidates[index_1++];
		else if (cap_getName(vote_1->candidates[index_1]) >
			 cap_getName(vote_2->candidates[index_2]))
			merged_vote->candidates[index_merged++] =
			    vote_2->candidates[index_2++];
	}

	merged_vote->length = index_merged;
	return merged_vote;
}

/*
 * Obtains the end instance which is contemporary to given event, or closest after event
 */
static Cap *adjacencyVote_getWitnessAncestor(Cap *
						     cap,
						     Event * event)
{
	Cap *current = cap;
	Cap *parent;

	// TODO:
	// If not event DAG, then have to choose parent
	// Tomorrow's an other day...
	while ((parent = cap_getParent(current))
	       && event_isDescendant
	              (event, cap_getEvent(parent)))
		current = parent;

	if (cap_getEvent(parent) == event)
		return parent;

	return current;
}

/*
 * Replaces the candidates of a vote with their parents
 */
static void adjacencyVote_goToParents(AdjacencyVote * vote,
				      Cap * generation)
{
	int index;
	Event *event = cap_getEvent(generation);

	for (index = 0; index < vote->length; index++)
		vote->candidates[index] =
		    adjacencyVote_getWitnessAncestor(vote->candidates
						     [index], event);

}

///////////////////////////////////////////////
// Adjacency vote table :
//      simple lookup table of AdjacencyVotes
///////////////////////////////////////////////

struct adjacency_vote_table_st {
	struct hashtable *table;
	struct List * computationFront;
};

static uint32_t hash_from_key_fn(const void *key)
{
	return (uint32_t) cap_getName((Cap *) key);
}

static int keys_equal_fn(const void *key1, const void *key2)
{
	return key1 == key2;
}

static void valueFree(void *value)
{
	adjacencyVote_destruct((AdjacencyVote *) value);
}

// Basic constructor
static AdjacencyVoteTable *adjacencyVoteTable_construct()
{
	AdjacencyVoteTable *table = st_calloc(1, sizeof(AdjacencyVoteTable));
	table->table =
	    create_hashtable(16, hash_from_key_fn, keys_equal_fn, NULL,
			     valueFree);
	table->computationFront =
	    constructZeroLengthList(1000, NULL);

	return table;
}

// Basic destructor
static void adjacencyVoteTable_destruct(AdjacencyVoteTable * table)
{
	hashtable_destroy(table->table, true, false);
	destructList(table->computationFront);
	free(table);
}

// Get votes for Cap
static AdjacencyVote *adjacencyVoteTable_getVote(Cap * cap,
						 AdjacencyVoteTable *
						 table)
{
	if (cap)
		return (AdjacencyVote *) hashtable_search(table->table,
							  (void *)
							  cap);
	else
		return NULL;
}

/*
 * Insert vote for Cap
 */
static void adjacencyVoteTable_recordVote(AdjacencyVoteTable * table,
					  Cap * cap,
					  AdjacencyVote * vote)
{
	AdjacencyVote * old_vote = hashtable_remove(table->table,
						 (void *) cap,
						 false);

#ifdef BEN_DEBUG
	assert(vote->length == 0 || vote->candidates);
	assert(vote->length != 1 || cap_getAdjacency(cap) || !adjacencyVoteTable_getVote(vote->candidates[0], table));
	//if (cap == (Cap *) 0x217b10)
	//	abort();
#endif
	if(listContains(table->computationFront, cap))
		listRemove(table->computationFront, cap);

	adjacencyVote_destruct(old_vote);
	hashtable_insert(table->table, (void *) cap,
			 (void *) vote);
}

/*
 * Check if node actually votes for something
 */
static bool adjacencyVoteTable_doesNotVote(Cap * cap, AdjacencyVoteTable * table) {
	AdjacencyVote * vote = adjacencyVoteTable_getVote(cap, table);

	return (vote == NULL || vote->length  == 0);
}

///////////////////////////////////////////////
// Filling in method
///////////////////////////////////////////////

/*
 * If possible register parent node into compuation front
 */
static void fillingIn_registerParent(Cap * cap,
				     AdjacencyVoteTable * table)
{
	Cap *parent = cap_getParent(cap);
	int32_t childIndex, childNumber;

	st_logInfo("Registering parent node %p\n", cap);

	if (parent == NULL)
		return;
	if (cap_getAdjacency(parent)) {
		fillingIn_registerParent(cap_getParent(cap), table);
		return;
	}

#ifdef BEN_DEBUG
	assert(cap_getAdjacency(parent) == NULL);
#endif

	childNumber = cap_getChildNumber(parent);

	for (childIndex = 0; childIndex < childNumber; childIndex++)
		if (adjacencyVoteTable_doesNotVote(cap_getChild(parent, childIndex), table))
			return;

	listAppend(table->computationFront, parent);
}

/*
 * Initiate propagation by counting bottom nodes as decided
 */
static void fillingIn_registerLeafCap(Cap * cap,
					      AdjacencyVoteTable * table)
{
	AdjacencyVote *vote;

	st_logInfo("Visiting leaf %p\n", cap);

	// Mark as decided
	vote = adjacencyVote_construct(1);
	vote->candidates[0] = cap_getAdjacency(cap);
	adjacencyVoteTable_recordVote(table, cap, vote);

	// Register parent
	fillingIn_registerParent(cap, table);
}

static void fillingIn_propagateAdjacencyDownwards(Cap *
						  cap,
						  Cap * partner,
						  AdjacencyVoteTable *
						  table);

/*
 * Propagate adjacency to all children
 */
static void fillingIn_propagateToChildren(Cap * cap,
					  Cap * partner,
					  AdjacencyVoteTable * table)
{
	int32_t childIndex;

	// Propagate to children if necessary
	for (childIndex = 0; childIndex < cap_getChildNumber(cap); childIndex++)
		fillingIn_propagateAdjacencyDownwards(cap_getChild
						      (cap,
						       childIndex),
						      partner, table);
}

/*
 * Register decision to remain without adjacency
 */
static void fillingIn_giveUp(Cap * cap,
			     AdjacencyVoteTable * table)
{
	AdjacencyVote *blank_vote;
	Cap * partner;

	if (cap == NULL || !adjacencyVoteTable_getVote(cap, table))
		return;

	// If forcing surrender through ancestral links
	if ((partner = cap_getAdjacency(cap))) {
		cap_breakAdjacency(cap);
		blank_vote = adjacencyVote_construct(0);
		adjacencyVoteTable_recordVote(table, partner, blank_vote);
		fillingIn_giveUp(partner, table);
	}

	// Create blank vote
	blank_vote = adjacencyVote_construct(0);
	adjacencyVoteTable_recordVote(table, cap, blank_vote);
}

static void fillingIn_processChildrenVote(Cap *
					  cap,
					  AdjacencyVoteTable * table);

/*
 * When joining a node to a partner, ensure that partner's parents are aware of this
 */
static void fillingIn_propagateToParents(Cap * child,
					 AdjacencyVoteTable * table)
{
	Cap *parent = cap_getParent(child);

#ifdef BEN_DEBUG
	assert(child != NULL);
#endif

	// Already at root
	if (parent == NULL)
		return;

	// Parent already happily paired up
	if (cap_getAdjacency(parent))
		return;

	// TODO: how about converting an old abstentionist?
	if (adjacencyVoteTable_doesNotVote(parent, table))
		return;

	// Update parent's vote
	fillingIn_processChildrenVote(parent, table);
}

/*
 * Create an interpolation between parent and child caps at event
 */
static Cap *fillingIn_interpolateCaps(Cap * parentCap,
					    Cap * childCap,
					    Event * event) {
	Cap * newCap = cap_construct(cap_getEnd(childCap), event);

	if (!cap_getOrientation(newCap))
		newCap = cap_getReverse(newCap);

#ifdef BEN_DEBUG
	st_logInfo("Interpolating %p\n", newCap);
	assert(event_isDescendant(cap_getEvent(parentCap), cap_getEvent(newCap)));
	assert(event_isDescendant(cap_getEvent(newCap), cap_getEvent(childCap)));
	assert(event_isDescendant(cap_getEvent(parentCap), cap_getEvent(childCap)));
#endif
	cap_changeParentAndChild(newCap, childCap);
	cap_makeParentAndChild(parentCap, newCap);

	return newCap;
}

/*
 * Make two end instances adjacent, and record the decision
 */
static void fillingIn_uniteCaps(Cap * cap,
					Cap * partner,
					AdjacencyVote * vote,
					AdjacencyVoteTable * table)
{
	AdjacencyVote * partnerVote;

	if (cap_getEvent(cap) != cap_getEvent(partner))
		partner = fillingIn_interpolateCaps(cap_getParent(partner), partner, cap_getEvent(cap));
	st_logInfo("A");

	cap_makeAdjacent(cap, partner);

	partnerVote = adjacencyVote_construct(1);
	partnerVote->candidates[0] = cap;
	adjacencyVoteTable_recordVote(table, partner, partnerVote);

	// End instance's vote
	vote = adjacencyVote_construct(1);
	vote->candidates[0] = partner;
	adjacencyVoteTable_recordVote(table, cap, vote);

	// Tell all the family
	fillingIn_propagateToChildren(partner, cap, table);
	fillingIn_propagateToParents(partner, table);
	fillingIn_propagateToChildren(cap, partner, table);
}

/*
 * Tests if a partner agrees to be paired up
 */
static bool fillingIn_partnerConsents(Cap * partner, Cap * cap, AdjacencyVoteTable * table) {
	AdjacencyVote *partner_vote = adjacencyVoteTable_getVote(partner, table);
	Cap * partnerPartner = cap_getAdjacency(partner);

	if (!partner_vote)
		return NULL;

	if (partnerPartner && partnerPartner != cap && !adjacencyVote_isDescendantOf(partnerPartner, cap, table))
		return NULL;

	return adjacencyVote_containsDescent(partner_vote, cap, table);
}

/*
 * Creates an inteprolation half way on the branch between two events
 */
static Event *fillingIn_interpolateEvents(Event* parentEvent, Event* childEvent) {
	float branchLength;
	EventTree * eventTree = event_getEventTree(parentEvent);
	Net * net = eventTree_getNet(eventTree);
	NetDisk * netDisk = net_getNetDisk(net);
	MetaEvent * metaEvent;
	Event * ptr = childEvent;
	Event * result;

#ifdef BEN_DEBUG
	assert(event_isDescendant(parentEvent, childEvent));
#endif

	// Compute total branch length
	for(ptr = childEvent; ptr != parentEvent; ptr = event_getParent(ptr))
		branchLength += event_getBranchLength(ptr);

	if (branchLength <= 1) {
		metaEvent = metaEvent_construct("interpolation", netDisk);
		return event_construct2(metaEvent, 0, event_getParent(childEvent), childEvent, eventTree);
	}

	// Compute desired branch length _from the bottom_
	branchLength /= 2;

	for(ptr = childEvent; ptr != parentEvent; ptr = event_getParent(ptr)) {
		if (event_getBranchLength(ptr) == branchLength) {
#ifdef BEN_DEBUG
			assert(event_isDescendant(parentEvent, event_getParent(ptr)));
#endif
			return event_getParent(ptr);
		}
		if (event_getBranchLength(ptr) > branchLength)
			break;

		branchLength -= event_getBranchLength(ptr);
#ifdef BEN_DEBUG
		assert(ptr);
#endif
	}

	metaEvent = metaEvent_construct("interpolation", netDisk);
	result = event_construct2(metaEvent, event_getBranchLength(ptr) - branchLength, event_getParent(ptr), ptr, eventTree);
#ifdef BEN_DEBUG
	assert(event_getBranchLength(result) > 0);
	assert(event_getBranchLength(ptr) > 0);
	assert(event_getBranchLength(event_getParent(ptr)) > 0);
	assert(event_isDescendant(parentEvent, ptr));
	assert(event_isDescendant(parentEvent, result));
#endif
	return result;
}

/*
 * Pair up a node with a null stub if all else fails
 */
static void fillingIn_pairUpToNullStub(Cap * cap, Cap * nonPartner, AdjacencyVoteTable * table) {

#ifdef BEN_DEBUG
	assert((cap_getParent(nonPartner) == NULL && cap_getParent(cap) == NULL) || (cap_getParent(nonPartner) && cap_getParent(cap)));
#endif

	if (event_isDescendant(cap_getEvent(cap), cap_getEvent(nonPartner))) {
		Cap * stub = fillingIn_interpolateCaps(cap_getParent(nonPartner), nonPartner, cap_getEvent(cap));
		Cap * child, *childPartner;
		AdjacencyVote * vote, *stubVote;
		int32_t childNumber, childIndex;

		st_logInfo("B");

		cap_makeAdjacent(cap, stub);

		// Create new votes bulletins to stuff the electoral box
		vote = adjacencyVote_construct(1);
		vote->candidates[0] = stub;
		adjacencyVoteTable_recordVote(table, cap, vote);

		stubVote = adjacencyVote_construct(1);
		stubVote->candidates[0] = cap;
		adjacencyVoteTable_recordVote(table, stub, stubVote);

		// Adjust children for consistency
		childNumber = cap_getChildNumber(cap);
		for (childIndex = 0; childIndex < childNumber; childIndex++) {
			child = cap_getChild(cap, childIndex);
			childPartner = cap_getAdjacency(child);

			if (childPartner && adjacencyVote_isDescendantOf(childPartner, nonPartner, table))
				cap_changeParentAndChild(stub, childPartner);
		}

		// Send info down
		fillingIn_propagateToChildren(cap, stub, table);
	} else if (cap_getParent(cap)) {
		Cap * parent, * nonPartnerParent;
		Cap * stub;
		Cap * parentStub, * nonPartnerParentStub;

		Cap * child, *childPartner;
		int32_t childNumber = cap_getChildNumber(cap);
		int32_t childIndex;
		AdjacencyVote * vote, *stubVote, *nonPartnerParentStubVote, *parentStubVote;

		Event * parentEvent;

		// Get parents
		parent = cap_getParent(cap);
		nonPartnerParent = cap_getParent(nonPartner);

		// Create pair
		stub = cap_construct(cap_getEnd(nonPartner), cap_getEvent(cap));
		st_logInfo("Constructing %p\n", stub);
		cap_makeAdjacent(cap, stub);

		// Create new votes bulletins to stuff the electoral box
		vote = adjacencyVote_construct(1);
		vote->candidates[0] = stub;
		adjacencyVoteTable_recordVote(table, cap, vote);

		stubVote = adjacencyVote_construct(1);
		stubVote->candidates[0] = cap;
		adjacencyVoteTable_recordVote(table, stub, stubVote);

		// Adjust children for consistency
		for (childIndex = 0; childIndex < childNumber; childIndex++) {
			child = cap_getChild(cap, childIndex);
			childPartner = cap_getAdjacency(child);

			if (childPartner && adjacencyVote_isDescendantOf(childPartner, nonPartner, table))
				cap_changeParentAndChild(stub, childPartner);
		}

#ifdef BEN_DEBUG
		assert(cap_getEvent(nonPartner) == cap_getEvent(cap));
		assert(!event_isDescendant(cap_getEvent(cap), cap_getEvent(cap_getParent(nonPartner))));
		assert(event_isDescendant(cap_getEvent(cap_getParent(nonPartner)), cap_getEvent(cap)));
		assert(event_isDescendant(cap_getEvent(nonPartnerParent), cap_getEvent(cap)));
		assert(event_isDescendant(cap_getEvent(parent), cap_getEvent(nonPartnerParent))
		       || event_isDescendant(cap_getEvent(nonPartnerParent), cap_getEvent(parent))
		       || cap_getEvent(nonPartnerParent) == cap_getEvent(parent));
#endif

		if (event_isDescendant(cap_getEvent(parent), cap_getEvent(nonPartnerParent)))
			parentEvent = fillingIn_interpolateEvents(cap_getEvent(nonPartnerParent), cap_getEvent(cap));
		else
			parentEvent = fillingIn_interpolateEvents(cap_getEvent(parent), cap_getEvent(cap));

		nonPartnerParentStub = fillingIn_interpolateCaps(nonPartnerParent, nonPartner, parentEvent);
		st_logInfo("C");
		cap_makeParentAndChild(nonPartnerParentStub, stub);

		parentStub = fillingIn_interpolateCaps(parent, cap, parentEvent);
		st_logInfo("D");
		cap_makeAdjacent(nonPartnerParentStub, parentStub);

		nonPartnerParentStubVote = adjacencyVote_construct(1);
		nonPartnerParentStubVote->candidates[0] = parentStub;
		adjacencyVoteTable_recordVote(table, nonPartnerParentStub, nonPartnerParentStubVote);

		parentStubVote = adjacencyVote_construct(1);
		parentStubVote->candidates[0] = nonPartnerParentStub;
		adjacencyVoteTable_recordVote(table, parentStub, parentStubVote);

		// Send info down
		fillingIn_propagateToChildren(cap, stub, table);

		fillingIn_registerParent(cap, table);
		fillingIn_registerParent(nonPartner, table);
	} else {
		// If working with root nodes the process is very similar, but must not be done
		// at the root event level so as not to break up a descent tree in to
		// It is therefore done just below
		Cap * nonPartnerPartner = cap_getAdjacency(nonPartner);
		Cap * stub, * nonPartnerStub, * partnerStub, *nonPartnerPartnerStub;
		Cap * child, *childPartner;
		Event * childEvent;
		int32_t childNumber;
		int32_t childIndex;
		AdjacencyVote * vote, *stubVote, *nonPartnerStubVote, *nonPartnerPartnerStubVote;

		childNumber = cap_getChildNumber(cap);
#ifdef BEN_DEBUG
		assert(nonPartnerPartner);
		assert(childNumber != 0);
#endif
		childEvent = cap_getEvent(cap_getChild(cap, 0));
		for (childIndex = 1; childIndex < childNumber; childIndex++) {
			child = cap_getChild(cap, childIndex);
			if (event_isDescendant(cap_getEvent(child), childEvent))
				childEvent = cap_getEvent(child);
		}

		childNumber = cap_getChildNumber(nonPartner);
#ifdef BEN_DEBUG
		assert(childNumber != 0);
#endif
		for (childIndex = 1; childIndex < childNumber; childIndex++) {
			child = cap_getChild(nonPartner, childIndex);
			if (event_isDescendant(cap_getEvent(child), childEvent))
				childEvent = cap_getEvent(child);
		}

		childNumber = cap_getChildNumber(nonPartnerPartner);
#ifdef BEN_DEBUG
		assert(childNumber != 0);
#endif
		for (childIndex = 1; childIndex < childNumber; childIndex++) {
			child = cap_getChild(nonPartnerPartner, childIndex);
			if (event_isDescendant(cap_getEvent(child), childEvent))
				childEvent = cap_getEvent(child);
		}

		childEvent = fillingIn_interpolateEvents(cap_getEvent(cap), childEvent);

		stub = cap_construct(cap_getEnd(cap), childEvent);
		st_logInfo("Constructing %p\n", stub);
		while(cap_getChildNumber(cap))
			cap_changeParentAndChild(stub, cap_getChild(cap, 0));
		cap_makeParentAndChild(cap, stub);

		partnerStub = cap_construct(cap_getEnd(nonPartner), childEvent);
		st_logInfo("Constructing %p\n", partnerStub);
		cap_makeAdjacent(stub, partnerStub);

		// Create new votes bulletins to stuff the electoral box
		vote = adjacencyVote_construct(1);
		vote->candidates[0] = stub;
		adjacencyVoteTable_recordVote(table, partnerStub, vote);

		stubVote = adjacencyVote_construct(1);
		stubVote->candidates[0] = partnerStub;
		adjacencyVoteTable_recordVote(table, stub, stubVote);

		nonPartnerPartnerStub = cap_construct(cap_getEnd(nonPartnerPartner), childEvent);
		st_logInfo("Constructing %p\n", nonPartnerPartnerStub);
		while(cap_getChildNumber(nonPartnerPartner))
			cap_changeParentAndChild(nonPartnerPartnerStub, cap_getChild(nonPartnerPartner, 0));
		cap_makeParentAndChild(nonPartnerPartner, nonPartnerPartnerStub);

		nonPartnerStub = cap_construct(cap_getEnd(nonPartner), childEvent);
		st_logInfo("Constructing %p\n", nonPartnerStub);
		cap_makeAdjacent(nonPartnerStub, nonPartnerPartnerStub);

		// Create new votes bulletins to stuff the electoral box
		nonPartnerStubVote = adjacencyVote_construct(1);
		vote->candidates[0] = nonPartnerPartnerStub;
		adjacencyVoteTable_recordVote(table, nonPartnerStub, nonPartnerStubVote);

		nonPartnerPartnerStubVote = adjacencyVote_construct(1);
		stubVote->candidates[0] = nonPartnerStub;
		adjacencyVoteTable_recordVote(table, nonPartnerPartnerStub, nonPartnerPartnerStubVote);

		// Split nonPartner's children according to divorce line
		childNumber = cap_getChildNumber(stub);
		for (childIndex = 0; childIndex < childNumber; childIndex++) {
			child = cap_getChild(stub, childIndex);
			childPartner = cap_getAdjacency(child);

			if (childPartner && adjacencyVote_isDescendantOf(childPartner, nonPartner, table))
				cap_changeParentAndChild(partnerStub, childPartner);
		}
		while(cap_getChildNumber(nonPartner))
			cap_changeParentAndChild(nonPartnerStub, cap_getChild(nonPartner, 0));

		cap_makeParentAndChild(nonPartner, partnerStub);
		cap_makeParentAndChild(nonPartner, nonPartnerStub);

		// Send info down
		fillingIn_propagateToChildren(stub, partnerStub, table);
	}
}

/*
 * Register decision to pick a candidate at random
 */
static void fillingIn_chooseAtRandom(Cap * cap,
				     AdjacencyVote * vote,
				     AdjacencyVoteTable * table)
{
	Cap *partner;
	int32_t index;

	for (index = 0; index < vote->length; index++) {
		partner = vote->candidates[index];
		partner = adjacencyVote_getWitnessAncestor(partner, cap_getEvent(cap));
		if (fillingIn_partnerConsents(partner, cap, table)) {
			fillingIn_uniteCaps(cap, partner,
						    vote, table);
			return;
		}
	}
}

/*
 * Determine adjacencies from the module pointed by the given Cap
 */
static void fillingIn_processChildrenVote(Cap *
					  cap,
					  AdjacencyVoteTable * table)
{
	int32_t childrenNumber = cap_getChildNumber(cap);
	Cap *first_child, *second_child;
	AdjacencyVote *first_child_vote, *second_child_vote, *merged_vote;
	AdjacencyVote *partner_vote = NULL;
	Cap *partner = NULL;

#ifdef BEN_DEBUG
	//Already paired up
	assert(cap_getAdjacency(cap) == NULL);
#endif

	// Consult children
	switch (childrenNumber) {
	case 0:
		// NOTE: This should not happen in theory since only
		// children can register their parents
#ifdef BEN_DEBUG
		assert(false);
#endif
		break;
	case 1:
		first_child = cap_getChild(cap, 0);
		merged_vote =
		    adjacencyVote_copy(adjacencyVoteTable_getVote
				       (first_child, table));
		adjacencyVote_goToParents(merged_vote, cap);
		break;
	case 2:
		first_child = cap_getChild(cap, 0);
		second_child = cap_getChild(cap, 1);

		first_child_vote =
		    adjacencyVote_copy(adjacencyVoteTable_getVote
				       (first_child, table));
		second_child_vote =
		    adjacencyVote_copy(adjacencyVoteTable_getVote
				       (second_child, table));
		adjacencyVote_goToParents(first_child_vote, cap);
		adjacencyVote_goToParents(second_child_vote, cap);
		merged_vote =
		    adjacencyVote_processVotes(first_child_vote,
					       second_child_vote);
		adjacencyVote_destruct(first_child_vote);
		adjacencyVote_destruct(second_child_vote);
		break;
	}


	// Make decision
	if (merged_vote->length == 1) {
		partner = adjacencyVote_getWinner(merged_vote);

		if (partner == NULL || !(partner_vote =
		     adjacencyVoteTable_getVote(partner, table))) {
			// If all children unattached or
			// Partner undecided... quizas, quizas, quizas
			fillingIn_propagateToParents(cap, table);
			adjacencyVoteTable_recordVote(table, cap,
						      merged_vote);
		} else if (fillingIn_partnerConsents(partner, cap, table))
			// Partner agrees!
			fillingIn_uniteCaps(cap, partner,
						    merged_vote, table);
		else {
			// Partner has someone else in their life...
			fillingIn_pairUpToNullStub(cap, partner, table);
			adjacencyVote_destruct(merged_vote);
		}
	}

	// If still undecided record alternatives
	else if (adjacencyVote_getLength(merged_vote) < VOTE_CUTOFF) {
		fillingIn_propagateToParents(cap, table);
		adjacencyVoteTable_recordVote(table, cap,
					      merged_vote);
	}

	// If necessary to make a decision because junction node
	else if (cap_getChildNumber(cap) > 1) {
		fillingIn_chooseAtRandom(cap, merged_vote, table);
		// If failure on junction: force to null stub
		if (!cap_getAdjacency(cap)) {
			partner = adjacencyVote_getWinner(merged_vote);
			fillingIn_pairUpToNullStub(cap, partner, table);
		}
		adjacencyVote_destruct(merged_vote);
	}

	// If too lazy to make a decision
	else {
		fillingIn_giveUp(cap, table);
		adjacencyVote_destruct(merged_vote);
	}
}

/*
 * Recursively apply second Fitch rule downwards when an ambiguity is resolved
 */
static void fillingIn_propagateAdjacencyDownwards(Cap *
						  cap,
						  Cap * partner,
						  AdjacencyVoteTable *
						  table)
{
	Cap *decision;
	AdjacencyVote *vote =
	    adjacencyVoteTable_getVote(cap, table);

#if BEN_DEBUG
	assert(vote->length >= 1 || cap_getAdjacency(cap));
#endif

	// If Cap does not exist (for NULL partners) or child already decided
	if (!cap || vote->length <= 1)
		return;

#ifdef BEN_DEBUG
	assert(partner != NULL);
#endif

	decision =
	    adjacencyVote_containsDescent(vote, partner, table);
	if (decision)
		decision = adjacencyVote_getWitnessAncestor(decision, cap_getEvent(cap));


	if (!decision || !fillingIn_partnerConsents(decision, cap, table))
		// If partner does not exist or refuses
		fillingIn_chooseAtRandom(cap, vote, table);
	else
		// Determined partner accepts
		fillingIn_uniteCaps(cap, decision, vote,
					    table);

	if (decision)
		decision = adjacencyVote_getWitnessAncestor(decision, cap_getEvent(cap));

	// If failure on junction: force to null stub
	if (!cap_getAdjacency(cap)) {
		if (cap_getChildNumber(cap) > 1) {
			if (decision && adjacencyVoteTable_getVote(decision, table))
				fillingIn_pairUpToNullStub(cap, decision, table);
			else {
				adjacencyVote_goToParents(vote, cap);
				decision = adjacencyVote_getWinner(vote);
				fillingIn_pairUpToNullStub(cap, decision, table);
			}
		}
	}

}

/*
 * Proagate along compution front
 */
static void fillingIn_stepForward(Cap * cap,
				  AdjacencyVoteTable * table)
{
	st_logInfo("Stepping into %p\n", cap);
	// Assess node decision
	fillingIn_processChildrenVote(cap, table);

	// Add parents to computationFront
	fillingIn_registerParent(cap, table);
}

/*
 * Force the top junction node in tree to come to a decision
 */
static void fillingIn_forceIndecisiveTree(Cap * cap, AdjacencyVoteTable * table) {
	AdjacencyVote * vote;
	Cap * partner;

	while(cap_getChildNumber(cap) == 1)
		cap = cap_getChild(cap, 0);

	// If non branched tree
	if (cap_getChildNumber(cap) == 0)
		return;

	// If attached, job done
	if (cap_getAdjacency(cap))
		return;

	// If not done, sort it out!!
	vote = adjacencyVoteTable_getVote(cap, table);
	fillingIn_chooseAtRandom(cap, vote, table);
	// If failure on junction: force to null stub
	if (!cap_getAdjacency(cap)) {
		adjacencyVote_goToParents(vote, cap);
		partner = adjacencyVote_getWinner(vote);
		fillingIn_pairUpToNullStub(cap, partner, table);
	}
}

/*
 * Go up to the first registered and attached ancestor (if exists)
 */
static Cap *fillingIn_getAttachedAncestor(Cap *
						  cap)
{
	Cap *current = cap_getParent(cap);
	Cap *parent;

	while (current && !cap_getAdjacency(cap)
	       && (parent = cap_getParent(current)))
		current = parent;

	return current;
}

/*
 * Builds a free stub end with the given group. The new end contains a cap with the given
 * event, and if the given event is not the root event, a root cap to attach the new
 * cap to the event.
 */
static Cap * fillingIn_constructStub(Event *event, Group *group) {
	EventTree *eventTree = event_getEventTree(event);
	End *newFreeStubEnd = end_construct(0, group_getNet(group));
	end_setGroup(newFreeStubEnd, group);
	Cap *cap = cap_construct(newFreeStubEnd, event);
		st_logInfo("Constructing %p\n",cap);
	Event *rootEvent = eventTree_getRootEvent(eventTree);
	if(event == rootEvent) {
		end_setRootInstance(newFreeStubEnd, cap);
	}
	else {
		end_setRootInstance(newFreeStubEnd, cap_construct(newFreeStubEnd, rootEvent));
		st_logInfo("Constructing %p\n", newFreeStubEnd);
		cap_makeParentAndChild(end_getRootInstance(newFreeStubEnd), cap);
	}
	return cap;
}

/*
 * Amend graph to remove lifted self loops
 */
static void fillingIn_resolveSelfLoop(Cap * ancestor,
				      Cap * descendant1,
				      Cap * descendant2)
{
	Cap *interpolation1, *interpolation2;
	Cap *alterEgoAdjacency1, *alterEgoAdjacency2;
	Cap *alterEgoParent;
	Cap *alterEgoChild;
	Cap *interpolationJoin;
	Event *bottomEvent = cap_getEvent(descendant1);
	Event *topEvent = cap_getEvent(ancestor);
	Group *group = end_getGroup(cap_getEnd(ancestor));
	Event *middleParentEvent, *middleChildEvent;

	// Move up on the event tree
	while (event_getParent(bottomEvent) != topEvent)
		bottomEvent = event_getParent(bottomEvent);
	middleChildEvent = fillingIn_interpolateEvents(topEvent, bottomEvent);
	middleParentEvent = fillingIn_interpolateEvents(topEvent, middleChildEvent);

	// Move up end instances so as to be right under ancestor
	while (cap_getParent(descendant1) != ancestor)
		descendant1 = cap_getParent(descendant1);

	// Same thing on branch 2
	while (cap_getParent(descendant2) != ancestor)
		descendant2 = cap_getParent(descendant2);

	// Modify top end of original tree
	interpolationJoin = fillingIn_interpolateCaps(ancestor, descendant1, middleParentEvent);
	st_logInfo("E");
	cap_changeParentAndChild(interpolationJoin, descendant2);

	interpolation1 =
	    fillingIn_interpolateCaps(interpolationJoin, descendant1,
				    middleChildEvent);
	interpolation2 =
	    fillingIn_interpolateCaps(interpolationJoin, descendant2,
				    middleChildEvent);

	// Create alter ego tree
	alterEgoParent = fillingIn_constructStub(topEvent, group); //construct the parent stub (and possible a root cap above it).
	End *newStubEnd = cap_getEnd(alterEgoParent);
	alterEgoChild = cap_construct(newStubEnd, cap_getEvent(interpolationJoin));
		st_logInfo("Constructing %p\n", alterEgoChild);
	alterEgoAdjacency1 = cap_construct(newStubEnd, middleChildEvent);
		st_logInfo("Constructing %p\n", alterEgoAdjacency1);
	alterEgoAdjacency2 = cap_construct(newStubEnd, middleChildEvent);
		st_logInfo("Constructing %p\n", alterEgoAdjacency2);

	cap_makeParentAndChild(alterEgoParent, alterEgoChild);
	cap_makeParentAndChild(alterEgoChild, alterEgoAdjacency1);
	cap_makeParentAndChild(alterEgoChild, alterEgoAdjacency2);


	// Join the two trees
	cap_makeAdjacent(interpolationJoin, alterEgoChild);
	cap_makeAdjacent(interpolation1, alterEgoAdjacency1);
	cap_makeAdjacent(interpolation2, alterEgoAdjacency2);
}

/*
 * Remove all lifted self loops from net
 */
static void fillingIn_removeSelfLoops(Net * net)
{
	Net_CapIterator *iter = net_getCapIterator(net);
	Cap *cap, *attachedAncestor;
	Cap *partner, *adjacencyAncestor;

	// Iterate through potential bottom nodes
	while ((cap = net_getNextCap(iter))) {
		// ... check if connected
		if ((partner = cap_getAdjacency(cap))) {
			// ... lift
			attachedAncestor =
			    fillingIn_getAttachedAncestor(cap);
			adjacencyAncestor =
			    fillingIn_getAttachedAncestor(partner);

			// Self loop
			if (adjacencyAncestor
			    && adjacencyAncestor == attachedAncestor)
				fillingIn_resolveSelfLoop(attachedAncestor,
							  cap,
							  partner);
		}
	}

	net_destructCapIterator(iter);
}

/*
 * Fill adjacencies in net using descent trees and event tree
 * Correct non-AVG anomalies
 */
void fillingIn_fillAdjacencies(Net * net)
{
	Cap *cap;
	AdjacencyVoteTable *table = adjacencyVoteTable_construct();
	Net_CapIterator *iter = net_getCapIterator(net);

	//////////////////////////////////////////////////////////////
	// Define a computation front
	//////////////////////////////////////////////////////////////
	st_logInfo("Registering leaf caps\n");
	while ((cap = net_getNextCap(iter))) {
		st_logInfo("Testing possible leaf %p\n", cap);
		if (cap_getChildNumber(cap) == 0)
			fillingIn_registerLeafCap(cap, table);
	}
	net_destructCapIterator(iter);

	//////////////////////////////////////////////////////////////
	// Compute greedily
	//////////////////////////////////////////////////////////////
	st_logInfo("Propagation\n");
	while (table->computationFront->length > 0)
		fillingIn_stepForward(listRemoveFirst(table->computationFront), table);

	//////////////////////////////////////////////////////////////
	// Force decision for higher nodes
	//////////////////////////////////////////////////////////////
	iter = net_getCapIterator(net);
	while ((cap = net_getNextCap(iter)))
		if (!cap_getParent(cap))
			fillingIn_forceIndecisiveTree(cap, table);
	net_destructCapIterator(iter);

	//////////////////////////////////////////////////////////////
	// Remove self loops
	//////////////////////////////////////////////////////////////
	st_logInfo("Removing self-loops\n");
	fillingIn_removeSelfLoops(net);

	//////////////////////////////////////////////////////////////
	// Clean up
	//////////////////////////////////////////////////////////////
	st_logInfo("Clean up\n");
	adjacencyVoteTable_destruct(table);
}

/*
 * Prints out information
 */
static void usage() {
	fprintf(stderr, "cactus_fillAdjacencies [net-names, ordered by order they should be processed], version 0.2\n");
	fprintf(stderr, "-a --logLevel : Set the log level\n");
	fprintf(stderr, "-c --netDisk : The location of the net disk directory\n");
	fprintf(stderr, "-e --tempDirRoot : The temp file root directory\n");
	fprintf(stderr, "-h --help : Print this help screen\n");
}

/*
 * Command line wrapper
 */
int main(int argc, char ** argv) {
	/*
	 * The script builds trees.
	 *
	 * (1) It reads the net.
	 *
	 * (2) It fill adjacencies
	 *
	 * (3) It copies the relevant augmented events into the event trees of its descendants.
	 *
	 */
	NetDisk *netDisk;
	Net *net;
	int32_t startTime;
	int32_t j;

	/*
	 * Arguments/options
	 */
	char * logLevelString = NULL;
	char * netDiskName = NULL;

	///////////////////////////////////////////////////////////////////////////
	// (0) Parse the inputs handed by genomeCactus.py / setup stuff.
	///////////////////////////////////////////////////////////////////////////

	while(1) {
		static struct option long_options[] = {
			{ "logLevel", required_argument, 0, 'a' },
			{ "netDisk", required_argument, 0, 'c' },
			{ "help", no_argument, 0, 'h' },
			{ 0, 0, 0, 0 }
		};

		int option_index = 0;

		int key = getopt_long(argc, argv, "a:c:h", long_options, &option_index);

		if(key == -1) {
			break;
		}

		switch(key) {
			case 'a':
				logLevelString = st_string_copy(optarg);
				break;
			case 'c':
				netDiskName = st_string_copy(optarg);
				break;
			case 'h':
				usage();
				return 0;
			default:
				usage();
				return 1;
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// (0) Check the inputs.
	///////////////////////////////////////////////////////////////////////////

	assert(logLevelString == NULL || strcmp(logLevelString, "INFO") == 0 || strcmp(logLevelString, "DEBUG") == 0);
	assert(netDiskName != NULL);
	//assert(tempFileRootDirectory != NULL);

	//////////////////////////////////////////////
	//Set up logging
	//////////////////////////////////////////////

	if(logLevelString != NULL && strcmp(logLevelString, "INFO") == 0) {
		st_setLogLevel(ST_LOGGING_INFO);
	}
	if(logLevelString != NULL && strcmp(logLevelString, "DEBUG") == 0) {
		st_setLogLevel(ST_LOGGING_DEBUG);
	}

	//////////////////////////////////////////////
	//Log (some of) the inputs
	//////////////////////////////////////////////

	st_logInfo("Net disk name : %s\n", netDiskName);

	//////////////////////////////////////////////
	//Load the database
	//////////////////////////////////////////////

	netDisk = netDisk_construct(netDiskName);
	st_logInfo("Set up the net disk\n");

	//////////////////////////////////////////////
	//For each net do tree building..
	//////////////////////////////////////////////

	for (j = optind; j < argc; j++) {
		const char *netName = argv[j];
		st_logInfo("Processing the net named: %s\n", netName);

		///////////////////////////////////////////////////////////////////////////
		// Parse the basic reconstruction problem
		///////////////////////////////////////////////////////////////////////////

		net = netDisk_getNet(netDisk, netMisc_stringToName(netName));
		assert(net_builtTrees(net)); //we must have already built the trees for the problem at this stage
		st_logInfo("Parsed the net to be refined\n");

		///////////////////////////////////////////////////////////////////////////
		// Do nothing if we have already built the faces.
		///////////////////////////////////////////////////////////////////////////

		if(net_builtFaces(net)) {
			st_logInfo("We have already built faces for net %s\n", netName);
			continue;
		}

		///////////////////////////////////////////////////////////////////////////
		// Do nothing if not a leaf.
		///////////////////////////////////////////////////////////////////////////

		if(!net_isLeaf(net)) {
			st_logInfo("We currently only build nets for terminal problems: %s\n", netName);
			continue;
		}
		assert(net_isTerminal(net));
		assert(net_getBlockNumber(net) == 0); //this should be true of the terminal problems.

		///////////////////////////////////////////////////////////////////////////
		// Fill adjencencies
		///////////////////////////////////////////////////////////////////////////

		startTime = time(NULL);
		fillingIn_fillAdjacencies(net);
		buildFaces_buildAndProcessFaces(net);
		st_logInfo("Processed the nets in: %i seconds\n", time(NULL) - startTime);

		///////////////////////////////////////////////////////////////////////////
		//Set the faces in the net to 'built' status, which triggers the building
		//of faces for the net.
		///////////////////////////////////////////////////////////////////////////

		assert(!net_builtFaces(net));
		net_setBuildFaces(net, 1);
	}

	///////////////////////////////////////////////////////////////////////////
	// (9) Write the net to disk.
	///////////////////////////////////////////////////////////////////////////

	startTime = time(NULL);
	netDisk_write(netDisk);
	st_logInfo("Updated the net on disk in: %i seconds\n", time(NULL) - startTime);

	///////////////////////////////////////////////////////////////////////////
	//(15) Clean up.
	///////////////////////////////////////////////////////////////////////////

	//Destruct stuff
	startTime = time(NULL);
	netDisk_destruct(netDisk);

	st_logInfo("Cleaned stuff up and am finished in: %i seconds\n", time(NULL) - startTime);
	return 0;

}
