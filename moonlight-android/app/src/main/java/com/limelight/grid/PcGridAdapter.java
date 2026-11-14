package com.limelight.grid;

import android.content.Context;
import android.view.View;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import com.limelight.PcView;
import com.limelight.R;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.PairingManager;
import com.limelight.preferences.PreferenceConfiguration;

import java.util.Collections;
import java.util.Comparator;

public class PcGridAdapter extends GenericGridAdapter<PcView.ComputerObject> {

    public PcGridAdapter(Context context, PreferenceConfiguration prefs) {
        super(context, getLayoutIdForPreferences(prefs));
    }

    private static int getLayoutIdForPreferences(PreferenceConfiguration prefs) {
        return R.layout.pc_grid_item;
    }

    public void updateLayoutWithPreferences(Context context, PreferenceConfiguration prefs) {
        // This will trigger the view to reload with the new layout
        setLayoutId(getLayoutIdForPreferences(prefs));
    }

    public void addComputer(PcView.ComputerObject computer) {
        itemList.add(computer);
        sortList();
    }

    // §REVIEW-1104：資料變更（如主機改名）時重新排序，避免畫面顯示新名稱但位置仍停在舊字母順序
    @Override
    public void notifyDataSetChanged() {
        sortList();
        super.notifyDataSetChanged();
    }

    // 排序與下面的 populateView 都只讀 ComputerObject 的 shown* 欄位（UI 執行緒在
    // PcView.updateComputer 同步的那一份），不直接讀輪詢執行緒會原地改的 details：
    // 這樣「有沒有變」的比對與實際畫出的內容永遠是同一份值。
    public void sortList() {
        Collections.sort(itemList, new Comparator<PcView.ComputerObject>() {
            @Override
            public int compare(PcView.ComputerObject lhs, PcView.ComputerObject rhs) {
                return lhs.shownName.toLowerCase().compareTo(rhs.shownName.toLowerCase());
            }
        });
    }

    public boolean removeComputer(PcView.ComputerObject computer) {
        return itemList.remove(computer);
    }

    @Override
    public void populateView(View parentView, ImageView imgView, ProgressBar prgView, TextView txtView, ImageView overlayView, PcView.ComputerObject obj) {
        imgView.setImageResource(R.drawable.ic_computer);
        if (obj.shownState == ComputerDetails.State.ONLINE) {
            imgView.setAlpha(1.0f);
        }
        else {
            imgView.setAlpha(0.4f);
        }

        if (obj.shownState == ComputerDetails.State.UNKNOWN) {
            prgView.setVisibility(View.VISIBLE);
        }
        else {
            prgView.setVisibility(View.INVISIBLE);
        }

        txtView.setText(obj.shownName);
        if (obj.shownState == ComputerDetails.State.ONLINE) {
            txtView.setAlpha(1.0f);
        }
        else {
            txtView.setAlpha(0.4f);
        }

        if (obj.shownState == ComputerDetails.State.OFFLINE) {
            overlayView.setImageResource(R.drawable.ic_pc_offline);
            overlayView.setAlpha(0.4f);
            overlayView.setVisibility(View.VISIBLE);
        }
        // We must check if the status is exactly online and unpaired
        // to avoid colliding with the loading spinner when status is unknown
        else if (obj.shownState == ComputerDetails.State.ONLINE &&
                obj.shownPairState == PairingManager.PairState.NOT_PAIRED) {
            overlayView.setImageResource(R.drawable.ic_lock);
            overlayView.setAlpha(1.0f);
            overlayView.setVisibility(View.VISIBLE);
        }
        else {
            overlayView.setVisibility(View.GONE);
        }

        // VipleStream H.5: capability badge.  parentView is the
        // root RelativeLayout from pc_grid_item.xml; the badge lives
        // inside the meta row.  Hidden when peer is vanilla Sunshine
        // / GFE (or when we haven't probed serverinfo yet, in which
        // case isVipleStreamPeer is false by default).
        View vipleBadge = parentView.findViewById(R.id.grid_viple_badge);
        if (vipleBadge != null) {
            vipleBadge.setVisibility(obj.shownViplePeer ? View.VISIBLE : View.GONE);
        }
    }
}
