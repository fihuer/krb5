/*
 * prof_tree.c --- these routines maintain the parse tree of the
 * 	config file.
 * 
 * All of the details of how the tree is stored is abstracted away in
 * this file; all of the other profile routines build, access, and
 * modify the tree via the accessor functions found in this file.
 *
 * Each node may represent either a relation or a section header.
 * 
 * A section header must have its value field set to 0, and may a one
 * or more child nodes, pointed to by first_child.
 * 
 * A relation has as its value a pointer to allocated memory
 * containing a string.  Its first_child pointer must be null.
 *
 */


#include <stdio.h>
#include <string.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <errno.h>
#include <ctype.h>

#include "prof_int.h"

struct profile_node {
	errcode_t	magic;
	char *name;
	char *value;
	int group_level;
	int final:1;		/* Indicate don't search next file */
	struct profile_node *first_child;
	struct profile_node *parent;
	struct profile_node *next, *prev;
};

#define CHECK_MAGIC(node) \
	  if ((node)->magic != PROF_MAGIC_NODE) \
		  return PROF_MAGIC_NODE;

/*
 * Free a node, and any children
 */
void profile_free_node(node)
	struct profile_node *node;
{
	struct profile_node *child, *next;

	if (node->magic != PROF_MAGIC_NODE)
		return;
	
	if (node->name)
		free(node->name);
	if (node->value)
		free(node->value);

	for (child=node->first_child; child; child = next) {
		next = child->next;
		profile_free_node(child);
	}
	node->magic = 0;
	
	free(node);
}

/*
 * Create a node
 */
errcode_t profile_create_node(name, value, ret_node)
	const char *name, *value;
	struct profile_node **ret_node;
{
	struct profile_node *new;

	new = malloc(sizeof(struct profile_node));
	if (!new)
		return ENOMEM;
	memset(new, 0, sizeof(struct profile_node));
	new->name = malloc(strlen(name)+1);
	if (new->name == 0) {
		profile_free_node(new);
		return ENOMEM;
	}
	strcpy(new->name, name);
	if (value) {
		new->value = malloc(strlen(value)+1);
		if (new->value == 0) {
			profile_free_node(new);
			return ENOMEM;
		}
		strcpy(new->value, value);
	}
	new->magic = PROF_MAGIC_NODE;

	*ret_node = new;
	return 0;
}

/*
 * This function verifies that all of the representation invarients of
 * the profile are true.  If not, we have a programming bug somewhere,
 * probably in this file.
 */
errcode_t profile_verify_node(node)
	struct profile_node *node;
{
	struct profile_node *p, *last;
	errcode_t	retval;

	CHECK_MAGIC(node);

	if (node->value && node->first_child)
		return PROF_SECTION_WITH_VALUE;

	last = 0;
	for (p = node->first_child; p; last = p, p = p->next) {
		if (p->prev != last)
			return PROF_BAD_LINK_LIST;
		if (last && (last->next != p))
			return PROF_BAD_LINK_LIST;
		if (node->group_level+1 != p->group_level)
			return PROF_BAD_GROUP_LVL;
		if (p->parent != node)
			return PROF_BAD_PARENT_PTR;
		retval = profile_verify_node(p);
		if (retval)
			return retval;
	}
	return 0;
}

/*
 * Add a node to a particular section
 */
errcode_t profile_add_node(section, name, value, ret_node)
	struct profile_node *section;
	const char *name, *value;
	struct profile_node **ret_node;
{
	errcode_t retval;
	struct profile_node *p, *last, *new;
	int	cmp = -1;

	CHECK_MAGIC(section);

	if (section->value)
		return PROF_ADD_NOT_SECTION;

	/*
	 * Find the place to insert the new node.  We look for the
	 * place *after* the last match of the node name, since 
	 * order matters.
	 */
	for (p=section->first_child, last = 0; p; last = p, p = p->next) {
		cmp = strcmp(p->name, name);
		if (cmp > 0)
			break;
	}
	retval = profile_create_node(name, value, &new);
	if (retval)
		return retval;
	new->group_level = section->group_level+1;
	new->parent = section;
	new->prev = last;
	new->next = p;
	if (p)
		p->prev = new;
	if (last)
		last->next = new;
	else
		section->first_child = new;
	if (ret_node)
		*ret_node = new;
	return 0;
}

/*
 * Set the final flag on a particular node.
 */
errcode_t profile_make_node_final(node)
	struct profile_node *node;
{
	CHECK_MAGIC(node);

	node->final = 1;
	return 0;
}

/*
 * Check the final flag on a node
 */
int profile_is_node_final(node)
	struct profile_node *node;
{
	return (node->final != 0);
}

/*
 * Iterate through the section, returning the relations which match
 * the given name.  If name is NULL, then interate through all the
 * relations in the section.  The first time this routine is called,
 * the state pointer must be null.  When this profile_find_node_relation()
 * returns, if the state pointer is non-NULL, then this routine should
 * be called again.
 *
 * The returned character string in value points to the stored
 * character string in the parse string.  Before this string value is
 * returned to a calling application (profile_find_node_relation is not an
 * exported interface), it should be strdup()'ed.
 */
errcode_t profile_find_node_relation(section, name, state, ret_name, value)
	struct profile_node *section;
	const char *name;
	void **state;
	char **ret_name, **value;
{
	struct profile_node *p;

	CHECK_MAGIC(section);
	p = *state;
	if (p) {
		CHECK_MAGIC(p);
	} else
		p = section->first_child;
	
	while (p) {
		if (((name == 0) || (strcmp(p->name, name) == 0)) &&
		    p->value) {
			if (value)
				*value = p->value;
			if (ret_name)
				*ret_name = p->name;
			break;
		}
		p = p->next;
	}
	if (p == 0) {
		*state = 0;
		return PROF_NO_RELATION;
	}
	/*
	 * OK, we've found one match; now let's try to find another
	 * one.  This way, if we return a non-zero state pointer,
	 * there's guaranteed to be another match that's returned.
	 */
	p = p->next;
	while (p) {
		if (((name == 0) || (strcmp(p->name, name) == 0)) &&
		    p->value)
			break;
		p = p->next;
	}
	*state = p;
	return 0;
}

/*
 * Iterate through the section, returning the subsections which match
 * the given name.  If name is NULL, then interate through all the
 * subsections in the section.  The first time this routine is called,
 * the state pointer must be null.  When this profile_find_node_subsection()
 * returns, if the state pointer is non-NULL, then this routine should
 * be called again.
 */
errcode_t profile_find_node_subsection(section, name, state, ret_name,
				       subsection)
	struct profile_node *section;
	const char *name;
	void **state;
	char **ret_name;
	struct profile_node **subsection;
{
	struct profile_node *p;

	CHECK_MAGIC(section);
	p = *state;
	if (p) {
		CHECK_MAGIC(p);
	} else
		p = section->first_child;
	
	while (p) {
		if (((name == 0) || (strcmp(p->name, name) == 0)) &&
		    (p->value == 0)) {
			if (subsection)
				*subsection = p;
			if (ret_name)
				*ret_name = p->name;
			break;
		}
		p = p->next;
	}
	if (p == 0) {
		*state = 0;
		return PROF_NO_SECTION;
	}
	/*
	 * OK, we've found one match; now let's try to find another
	 * one.  This way, if we return a non-zero state pointer,
	 * there's guaranteed to be another match that's returned.
	 */
	p = p->next;
	while (p) {
		if (((name == 0) || (strcmp(p->name, name) == 0))
		    && (p->value == 0))
			break;
		p = p->next;
	}
	*state = p;
	return 0;
}

/*
 * This function deletes a subsection or relation from a section,
 * depending whether on the section flag is non-zero or not.
 */
errcode_t profile_remove_node(section, name, section_flag)
	struct profile_node *section;
	const char *name;
	int section_flag;
{
	struct profile_node *p, *next;
	
	for (p = section->first_child; p; p = p->next) {
		if ((strcmp(p->name, name) == 0) && p->value)
			break;
	}
	if (p == 0)
		return PROF_NO_RELATION;
	/*
	 * Now we start deleting the relations or subsection.
	 */
	while (p && (strcmp(p->name, name) == 0)) {
		/*
		 * Skip if it is not the correct type.
		 */
		if ((section_flag && p->value) ||
		    (!section_flag && !p->value)) {
			p = p->next;
			continue;
		}
		if (p->prev)
			p->prev->next = p->next;
		else
			section->first_child = p->next;
		next = p->next;
		if (p->next)
			p->next->prev = p->prev;
		profile_free_node(p);
		p = next;
	}
	return 0;
}

/*
 * This function returns the parent of a particular node.
 */
errcode_t profile_get_node_parent(section, parent)
	struct profile_node *section, **parent;
{
	*parent = section->parent;
	return 0;
}

/*
 * This is a general-purpose iterator for returning all nodes that
 * match the specified name array.  
 */
struct profile_iterator {
	prf_magic_t		magic;
	profile_t		profile;
	int			flags;
	const char 		**names;
	const char		*name;
	prf_file_t		file;
	int			file_serial;
	int			done_idx;
	struct profile_node 	*node;
	int			num;
};

errcode_t profile_node_iterator_create(profile, names, flags, ret_iter)
	profile_t	profile;
	const char	**names;
	int		flags;
	void		**ret_iter;
{
	struct profile_iterator *iter;
	int	done_idx = 0;

	if (profile == 0)
		return PROF_NO_PROFILE;
	if (profile->magic != PROF_MAGIC_PROFILE)
		return PROF_MAGIC_PROFILE;
	if (!names)
		return PROF_BAD_NAMESET;
	if (!(flags & PROFILE_ITER_LIST_SECTION)) {
		if (!names[0])
			return PROF_BAD_NAMESET;
		done_idx = 1;
	}

	if ((iter = malloc(sizeof(struct profile_iterator))) == NULL)
		return ENOMEM;

	iter->magic = PROF_MAGIC_ITERATOR;
	iter->profile = profile;
	iter->names = names;
	iter->flags = flags;
	iter->file = profile->first_file;
	iter->done_idx = done_idx;
	iter->node = 0;
	iter->num = 0;
	*ret_iter = iter;
	return 0;
}

void profile_node_iterator_free(iter_p)
	void	**iter_p;
{
	struct profile_iterator *iter;

	if (!iter_p)
		return;
	iter = *iter_p;
	if (!iter || iter->magic != PROF_MAGIC_ITERATOR)
		return;
	free(iter);
	*iter_p = 0;
}

/*
 * Note: the returned character strings in ret_name and ret_value
 * points to the stored character string in the parse string.  Before
 * this string value is returned to a calling application
 * (profile_node_iterator is not an exported interface), it should be
 * strdup()'ed.
 */
errcode_t profile_node_iterator(iter_p, ret_node, ret_name, ret_value)
	void	**iter_p;
	struct profile_node	**ret_node;
	char **ret_name, **ret_value;
{
	struct profile_iterator 	*iter = *iter_p;
	struct profile_node 		*section, *p;
	const char			**cpp;
	errcode_t			retval;
	int				skip_num = 0;

	if (!iter || iter->magic != PROF_MAGIC_ITERATOR)
		return PROF_MAGIC_ITERATOR;
	/*
	 * If the file has changed, then the node pointer is invalid,
	 * so we'll have search the file again looking for it.
	 */
	if (iter->node && (iter->file->upd_serial != iter->file_serial)) {
		iter->flags &= ~PROFILE_ITER_FINAL_SEEN;
		skip_num = iter->num;
		iter->node = 0;
	}
get_new_file:
	while (iter->node == 0) {
		if (iter->file == 0 ||
		    (iter->flags & PROFILE_ITER_FINAL_SEEN)) {
			profile_node_iterator_free(iter_p);
			if (ret_node)
				*ret_node = 0;
			if (ret_name)
				*ret_name = 0;
			if (ret_value)
				*ret_value =0;
			return 0;
		}
		if ((retval = profile_update_file(iter->file))) {
			profile_node_iterator_free(iter_p);
			return retval;
		}
		iter->file_serial = iter->file->upd_serial;
		/*
		 * Find the section to list if we are a LIST_SECTION,
		 * or find the containing section if not.
		 */
		section = iter->file->root;
		for (cpp = iter->names; cpp[iter->done_idx]; cpp++) {
			for (p=section->first_child; p; p = p->next)
				if (!strcmp(p->name, *cpp) && !p->value)
					break;
			if (!p) {
				section = 0;
				break;
			}
			section = p;
			if (p->final)
				iter->flags |= PROFILE_ITER_FINAL_SEEN;
		}
		if (!section) {
			iter->file = iter->file->next;
			skip_num = 0;
			continue;
		}
		iter->name = *cpp;
		iter->node = section->first_child;
	}
	/*
	 * OK, now we know iter->node is set up correctly.  Let's do
	 * the search.
	 */
	for (p = iter->node; p; p = p->next) {
		if (iter->name && strcmp(p->name, iter->name))
			continue;
		if ((iter->flags & PROFILE_ITER_SECTIONS_ONLY) &&
		    p->value)
			continue;
		if ((iter->flags & PROFILE_ITER_RELATIONS_ONLY) &&
		    !p->value)
			continue;
		if (skip_num > 0) {
			skip_num--;
			continue;
		}
		break;
	}
	iter->num++;
	if (!p) {
		iter->file = iter->file->next;
		iter->node = 0;
		skip_num = 0;
		goto get_new_file;
	}
	if ((iter->node = p->next) == NULL)
		iter->file = iter->file->next;
	if (ret_node)
		*ret_node = p;
	if (ret_name)
		*ret_name = p->name;
	if (ret_value)
		*ret_value = p->value;
	return 0;
};
