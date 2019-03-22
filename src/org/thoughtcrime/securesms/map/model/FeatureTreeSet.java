package org.thoughtcrime.securesms.map.model;

import android.support.annotation.NonNull;

import com.google.gson.JsonSyntaxException;
import com.mapbox.geojson.Feature;
import com.mapbox.geojson.Point;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.TreeSet;

/**
 * Created by cyberta on 21.03.19.
 */

public class FeatureTreeSet extends TreeSet<TimeComparableFeature> {


    public boolean replace(@NonNull TimeComparableFeature obj) {
        boolean existed = remove(obj);
        add(obj);
        return existed;
    }

    public ArrayList<Feature> getFeatureList() {
        ArrayList<Feature> featureList = new ArrayList<>();
        Iterator<TimeComparableFeature> iterator = this.iterator();
        while (iterator.hasNext()) {
            TimeComparableFeature timeComparableFeature = iterator.next();
            featureList.add(timeComparableFeature.getFeature());
        }
        return featureList;
    }

    public List<Point> getPointList() {
        ArrayList<Point> points = new ArrayList<>();
        Iterator<TimeComparableFeature> iterator = this.iterator();
        while (iterator.hasNext()) {
            TimeComparableFeature timeComparableFeature = iterator.next();
            try {
                points.add(Point.fromJson(timeComparableFeature.getFeature().geometry().toJson()));
            } catch (JsonSyntaxException | NullPointerException e) {
                e.printStackTrace();
            }
        }
        return points;
    }
}
