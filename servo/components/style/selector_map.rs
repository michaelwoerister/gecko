/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! A data structure to efficiently index structs containing selectors by local
//! name, ids and hash.

use crate::applicable_declarations::ApplicableDeclarationList;
use crate::context::QuirksMode;
use crate::dom::TElement;
use crate::hash::map as hash_map;
use crate::hash::{HashMap, HashSet};
use crate::rule_tree::{CascadeLevel, ShadowCascadeOrder};
use crate::selector_parser::SelectorImpl;
use crate::stylist::Rule;
use crate::{Atom, LocalName, Namespace, WeakAtom};
use fallible::FallibleVec;
use hashglobe::FailedAllocationError;
use precomputed_hash::PrecomputedHash;
use selectors::matching::{matches_selector, ElementSelectorFlags, MatchingContext};
use selectors::parser::{Combinator, Component, SelectorIter};
use smallvec::SmallVec;
use std::hash::{BuildHasherDefault, Hash, Hasher};

/// A hasher implementation that doesn't hash anything, because it expects its
/// input to be a suitable u32 hash.
pub struct PrecomputedHasher {
    hash: Option<u32>,
}

impl Default for PrecomputedHasher {
    fn default() -> Self {
        Self { hash: None }
    }
}

/// A simple alias for a hashmap using PrecomputedHasher.
pub type PrecomputedHashMap<K, V> = HashMap<K, V, BuildHasherDefault<PrecomputedHasher>>;

/// A simple alias for a hashset using PrecomputedHasher.
pub type PrecomputedHashSet<K> = HashSet<K, BuildHasherDefault<PrecomputedHasher>>;

impl Hasher for PrecomputedHasher {
    #[inline]
    fn write(&mut self, _: &[u8]) {
        unreachable!(
            "Called into PrecomputedHasher with something that isn't \
             a u32"
        )
    }

    #[inline]
    fn write_u32(&mut self, i: u32) {
        debug_assert!(self.hash.is_none());
        self.hash = Some(i);
    }

    #[inline]
    fn finish(&self) -> u64 {
        self.hash.expect("PrecomputedHasher wasn't fed?") as u64
    }
}

/// A trait to abstract over a given selector map entry.
pub trait SelectorMapEntry: Sized + Clone {
    /// Gets the selector we should use to index in the selector map.
    fn selector(&self) -> SelectorIter<SelectorImpl>;
}

/// Map element data to selector-providing objects for which the last simple
/// selector starts with them.
///
/// e.g.,
/// "p > img" would go into the set of selectors corresponding to the
/// element "img"
/// "a .foo .bar.baz" would go into the set of selectors corresponding to
/// the class "bar"
///
/// Because we match selectors right-to-left (i.e., moving up the tree
/// from an element), we need to compare the last simple selector in the
/// selector with the element.
///
/// So, if an element has ID "id1" and classes "foo" and "bar", then all
/// the rules it matches will have their last simple selector starting
/// either with "#id1" or with ".foo" or with ".bar".
///
/// Hence, the union of the rules keyed on each of element's classes, ID,
/// element name, etc. will contain the Selectors that actually match that
/// element.
///
/// We use a 1-entry SmallVec to avoid a separate heap allocation in the case
/// where we only have one entry, which is quite common. See measurements in:
/// * https://bugzilla.mozilla.org/show_bug.cgi?id=1363789#c5
/// * https://bugzilla.mozilla.org/show_bug.cgi?id=681755
///
/// TODO: Tune the initial capacity of the HashMap
#[derive(Debug, MallocSizeOf)]
pub struct SelectorMap<T: 'static> {
    /// Rules that have `:root` selectors.
    pub root: SmallVec<[T; 1]>,
    /// A hash from an ID to rules which contain that ID selector.
    pub id_hash: MaybeCaseInsensitiveHashMap<Atom, SmallVec<[T; 1]>>,
    /// A hash from a class name to rules which contain that class selector.
    pub class_hash: MaybeCaseInsensitiveHashMap<Atom, SmallVec<[T; 1]>>,
    /// A hash from local name to rules which contain that local name selector.
    pub local_name_hash: PrecomputedHashMap<LocalName, SmallVec<[T; 1]>>,
    /// A hash from namespace to rules which contain that namespace selector.
    pub namespace_hash: PrecomputedHashMap<Namespace, SmallVec<[T; 1]>>,
    /// All other rules.
    pub other: SmallVec<[T; 1]>,
    /// The number of entries in this map.
    pub count: usize,
}

impl<T: 'static> Default for SelectorMap<T> {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

// FIXME(Manishearth) the 'static bound can be removed when
// our HashMap fork (hashglobe) is able to use NonZero,
// or when stdlib gets fallible collections
impl<T: 'static> SelectorMap<T> {
    /// Trivially constructs an empty `SelectorMap`.
    pub fn new() -> Self {
        SelectorMap {
            root: SmallVec::new(),
            id_hash: MaybeCaseInsensitiveHashMap::new(),
            class_hash: MaybeCaseInsensitiveHashMap::new(),
            local_name_hash: HashMap::default(),
            namespace_hash: HashMap::default(),
            other: SmallVec::new(),
            count: 0,
        }
    }

    /// Clears the hashmap retaining storage.
    pub fn clear(&mut self) {
        self.root.clear();
        self.id_hash.clear();
        self.class_hash.clear();
        self.local_name_hash.clear();
        self.namespace_hash.clear();
        self.other.clear();
        self.count = 0;
    }

    /// Returns whether there are any entries in the map.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Returns the number of entries.
    pub fn len(&self) -> usize {
        self.count
    }
}

impl SelectorMap<Rule> {
    /// Append to `rule_list` all Rules in `self` that match element.
    ///
    /// Extract matching rules as per element's ID, classes, tag name, etc..
    /// Sort the Rules at the end to maintain cascading order.
    pub fn get_all_matching_rules<E, F>(
        &self,
        element: E,
        rule_hash_target: E,
        matching_rules_list: &mut ApplicableDeclarationList,
        context: &mut MatchingContext<E::Impl>,
        flags_setter: &mut F,
        cascade_level: CascadeLevel,
        shadow_cascade_order: ShadowCascadeOrder,
    ) where
        E: TElement,
        F: FnMut(&E, ElementSelectorFlags),
    {
        if self.is_empty() {
            return;
        }

        let quirks_mode = context.quirks_mode();

        if rule_hash_target.is_root() {
            SelectorMap::get_matching_rules(
                element,
                &self.root,
                matching_rules_list,
                context,
                flags_setter,
                cascade_level,
                shadow_cascade_order,
            );
        }

        if let Some(id) = rule_hash_target.id() {
            if let Some(rules) = self.id_hash.get(id, quirks_mode) {
                SelectorMap::get_matching_rules(
                    element,
                    rules,
                    matching_rules_list,
                    context,
                    flags_setter,
                    cascade_level,
                    shadow_cascade_order,
                )
            }
        }

        rule_hash_target.each_class(|class| {
            if let Some(rules) = self.class_hash.get(&class, quirks_mode) {
                SelectorMap::get_matching_rules(
                    element,
                    rules,
                    matching_rules_list,
                    context,
                    flags_setter,
                    cascade_level,
                    shadow_cascade_order,
                )
            }
        });

        if let Some(rules) = self.local_name_hash.get(rule_hash_target.local_name()) {
            SelectorMap::get_matching_rules(
                element,
                rules,
                matching_rules_list,
                context,
                flags_setter,
                cascade_level,
                shadow_cascade_order,
            )
        }

        if let Some(rules) = self.namespace_hash.get(rule_hash_target.namespace()) {
            SelectorMap::get_matching_rules(
                element,
                rules,
                matching_rules_list,
                context,
                flags_setter,
                cascade_level,
                shadow_cascade_order,
            )
        }

        SelectorMap::get_matching_rules(
            element,
            &self.other,
            matching_rules_list,
            context,
            flags_setter,
            cascade_level,
            shadow_cascade_order,
        );
    }

    /// Adds rules in `rules` that match `element` to the `matching_rules` list.
    pub (crate) fn get_matching_rules<E, F>(
        element: E,
        rules: &[Rule],
        matching_rules: &mut ApplicableDeclarationList,
        context: &mut MatchingContext<E::Impl>,
        flags_setter: &mut F,
        cascade_level: CascadeLevel,
        shadow_cascade_order: ShadowCascadeOrder,
    ) where
        E: TElement,
        F: FnMut(&E, ElementSelectorFlags),
    {
        for rule in rules {
            if matches_selector(
                &rule.selector,
                0,
                Some(&rule.hashes),
                &element,
                context,
                flags_setter,
            ) {
                matching_rules.push(
                    rule.to_applicable_declaration_block(cascade_level, shadow_cascade_order),
                );
            }
        }
    }
}

impl<T: SelectorMapEntry> SelectorMap<T> {
    /// Inserts into the correct hash, trying id, class, localname and
    /// namespace.
    pub fn insert(
        &mut self,
        entry: T,
        quirks_mode: QuirksMode,
    ) -> Result<(), FailedAllocationError> {
        self.count += 1;

        let vector = match find_bucket(entry.selector()) {
            Bucket::Root => &mut self.root,
            Bucket::ID(id) => self
                .id_hash
                .try_entry(id.clone(), quirks_mode)?
                .or_insert_with(SmallVec::new),
            Bucket::Class(class) => self
                .class_hash
                .try_entry(class.clone(), quirks_mode)?
                .or_insert_with(SmallVec::new),
            Bucket::LocalName { name, lower_name } => {
                // If the local name in the selector isn't lowercase, insert it
                // into the rule hash twice. This means that, during lookup, we
                // can always find the rules based on the local name of the
                // element, regardless of whether it's an html element in an
                // html document (in which case we match against lower_name) or
                // not (in which case we match against name).
                //
                // In the case of a non-html-element-in-html-document with a
                // lowercase localname and a non-lowercase selector, the
                // rulehash lookup may produce superfluous selectors, but the
                // subsequent selector matching work will filter them out.
                if name != lower_name {
                    self.local_name_hash
                        .try_entry(lower_name.clone())?
                        .or_insert_with(SmallVec::new)
                        .try_push(entry.clone())?;
                }
                self.local_name_hash
                    .try_entry(name.clone())?
                    .or_insert_with(SmallVec::new)
            },
            Bucket::Namespace(url) => self
                .namespace_hash
                .try_entry(url.clone())?
                .or_insert_with(SmallVec::new),
            Bucket::Universal => &mut self.other,
        };

        vector.try_push(entry)
    }

    /// Looks up entries by id, class, local name, namespace, and other (in
    /// order).
    ///
    /// Each entry is passed to the callback, which returns true to continue
    /// iterating entries, or false to terminate the lookup.
    ///
    /// Returns false if the callback ever returns false.
    ///
    /// FIXME(bholley) This overlaps with SelectorMap<Rule>::get_all_matching_rules,
    /// but that function is extremely hot and I'd rather not rearrange it.
    #[inline]
    pub fn lookup<'a, E, F>(&'a self, element: E, quirks_mode: QuirksMode, mut f: F) -> bool
    where
        E: TElement,
        F: FnMut(&'a T) -> bool,
    {
        if element.is_root() {
            for entry in self.root.iter() {
                if !f(&entry) {
                    return false;
                }
            }
        }

        if let Some(id) = element.id() {
            if let Some(v) = self.id_hash.get(id, quirks_mode) {
                for entry in v.iter() {
                    if !f(&entry) {
                        return false;
                    }
                }
            }
        }

        let mut done = false;
        element.each_class(|class| {
            if !done {
                if let Some(v) = self.class_hash.get(class, quirks_mode) {
                    for entry in v.iter() {
                        if !f(&entry) {
                            done = true;
                            return;
                        }
                    }
                }
            }
        });
        if done {
            return false;
        }

        if let Some(v) = self.local_name_hash.get(element.local_name()) {
            for entry in v.iter() {
                if !f(&entry) {
                    return false;
                }
            }
        }

        if let Some(v) = self.namespace_hash.get(element.namespace()) {
            for entry in v.iter() {
                if !f(&entry) {
                    return false;
                }
            }
        }

        for entry in self.other.iter() {
            if !f(&entry) {
                return false;
            }
        }

        true
    }

    /// Performs a normal lookup, and also looks up entries for the passed-in
    /// id and classes.
    ///
    /// Each entry is passed to the callback, which returns true to continue
    /// iterating entries, or false to terminate the lookup.
    ///
    /// Returns false if the callback ever returns false.
    #[inline]
    pub fn lookup_with_additional<'a, E, F>(
        &'a self,
        element: E,
        quirks_mode: QuirksMode,
        additional_id: Option<&WeakAtom>,
        additional_classes: &[Atom],
        mut f: F,
    ) -> bool
    where
        E: TElement,
        F: FnMut(&'a T) -> bool,
    {
        // Do the normal lookup.
        if !self.lookup(element, quirks_mode, |entry| f(entry)) {
            return false;
        }

        // Check the additional id.
        if let Some(id) = additional_id {
            if let Some(v) = self.id_hash.get(id, quirks_mode) {
                for entry in v.iter() {
                    if !f(&entry) {
                        return false;
                    }
                }
            }
        }

        // Check the additional classes.
        for class in additional_classes {
            if let Some(v) = self.class_hash.get(class, quirks_mode) {
                for entry in v.iter() {
                    if !f(&entry) {
                        return false;
                    }
                }
            }
        }

        true
    }
}

enum Bucket<'a> {
    Root,
    ID(&'a Atom),
    Class(&'a Atom),
    LocalName {
        name: &'a LocalName,
        lower_name: &'a LocalName,
    },
    Namespace(&'a Namespace),
    Universal,
}

fn specific_bucket_for<'a>(component: &'a Component<SelectorImpl>) -> Bucket<'a> {
    match *component {
        Component::Root => Bucket::Root,
        Component::ID(ref id) => Bucket::ID(id),
        Component::Class(ref class) => Bucket::Class(class),
        Component::LocalName(ref selector) => Bucket::LocalName {
            name: &selector.name,
            lower_name: &selector.lower_name,
        },
        Component::Namespace(_, ref url) | Component::DefaultNamespace(ref url) => {
            Bucket::Namespace(url)
        },
        // ::slotted(..) isn't a normal pseudo-element, so we can insert it on
        // the rule hash normally without much problem. For example, in a
        // selector like:
        //
        //   div::slotted(span)::before
        //
        // It looks like:
        //
        //  [
        //    LocalName(div),
        //    Combinator(SlotAssignment),
        //    Slotted(span),
        //    Combinator::PseudoElement,
        //    PseudoElement(::before),
        //  ]
        //
        // So inserting `span` in the rule hash makes sense since we want to
        // match the slotted <span>.
        Component::Slotted(ref selector) => find_bucket(selector.iter()),
        Component::Host(Some(ref selector)) => find_bucket(selector.iter()),
        _ => Bucket::Universal,
    }
}

/// Searches a compound selector from left to right, and returns the appropriate
/// bucket for it.
#[inline(always)]
fn find_bucket<'a>(mut iter: SelectorIter<'a, SelectorImpl>) -> Bucket<'a> {
    let mut current_bucket = Bucket::Universal;

    loop {
        // We basically want to find the most specific bucket,
        // where:
        //
        //   root > id > class > local name > namespace > universal.
        //
        for ss in &mut iter {
            let new_bucket = specific_bucket_for(ss);
            match new_bucket {
                Bucket::Root => return new_bucket,
                Bucket::ID(..) => {
                    current_bucket = new_bucket;
                },
                Bucket::Class(..) => {
                    if !matches!(current_bucket, Bucket::ID(..)) {
                        current_bucket = new_bucket;
                    }
                },
                Bucket::LocalName { .. } => {
                    if matches!(current_bucket, Bucket::Universal | Bucket::Namespace(..)) {
                        current_bucket = new_bucket;
                    }
                },
                Bucket::Namespace(..) => {
                    if matches!(current_bucket, Bucket::Universal) {
                        current_bucket = new_bucket;
                    }
                },
                Bucket::Universal => {},
            }
        }

        // Effectively, pseudo-elements are ignored, given only state
        // pseudo-classes may appear before them.
        if iter.next_sequence() != Some(Combinator::PseudoElement) {
            break;
        }
    }

    return current_bucket;
}

/// Wrapper for PrecomputedHashMap that does ASCII-case-insensitive lookup in quirks mode.
#[derive(Debug, MallocSizeOf)]
pub struct MaybeCaseInsensitiveHashMap<K: PrecomputedHash + Hash + Eq, V: 'static>(
    PrecomputedHashMap<K, V>,
);

// FIXME(Manishearth) the 'static bound can be removed when
// our HashMap fork (hashglobe) is able to use NonZero,
// or when stdlib gets fallible collections
impl<V: 'static> MaybeCaseInsensitiveHashMap<Atom, V> {
    /// Empty map
    pub fn new() -> Self {
        MaybeCaseInsensitiveHashMap(PrecomputedHashMap::default())
    }

    /// HashMap::try_entry
    pub fn try_entry(
        &mut self,
        mut key: Atom,
        quirks_mode: QuirksMode,
    ) -> Result<hash_map::Entry<Atom, V>, FailedAllocationError> {
        if quirks_mode == QuirksMode::Quirks {
            key = key.to_ascii_lowercase()
        }
        self.0.try_entry(key)
    }

    /// HashMap::iter
    pub fn iter(&self) -> hash_map::Iter<Atom, V> {
        self.0.iter()
    }

    /// HashMap::clear
    pub fn clear(&mut self) {
        self.0.clear()
    }

    /// HashMap::get
    pub fn get(&self, key: &WeakAtom, quirks_mode: QuirksMode) -> Option<&V> {
        if quirks_mode == QuirksMode::Quirks {
            self.0.get(&key.to_ascii_lowercase())
        } else {
            self.0.get(key)
        }
    }
}
